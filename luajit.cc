#include "kingtaker.hh"

#include <atomic>
#include <cinttypes>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>

#include <lua.hpp>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <ImNodes.h>

#include "iface/dir.hh"
#include "iface/factory.hh"
#include "iface/node.hh"

#include "util/gui.hh"
#include "util/notify.hh"
#include "util/ptr_selector.hh"
#include "util/value.hh"


namespace kingtaker {
namespace {

// LuaJIT device
class LuaJIT final {
 public:
  using Command = std::function<void(lua_State* L)>;

  class RuntimeException : public HeavyException {
   public:
    RuntimeException(std::string_view msg, Loc loc = Loc::current()) noexcept :
        HeavyException(msg, loc) {
    }
    std::string Stringify() const noexcept {
      return "[LuaJIT Exception]\n"s+Exception::Stringify();
    }
  };

  LuaJIT() noexcept {
    th_ = std::thread([this]() { Main(); });
  }
  ~LuaJIT() noexcept {
    alive_ = false;
    cv_.notify_all();
    th_.join();
  }

  void Queue(Command&& cmd) noexcept {
    std::unique_lock<std::mutex> k(mtx_);
    cmds_.push_back(std::move(cmd));
    cv_.notify_all();
  }

  // the first arg is not used but necessary
  // to make it ensure to be called from lua thread
  int SandboxCall(lua_State*, int narg, int nret) const noexcept {
    static constexpr size_t kSandboxInstructionLimit = 10000000;

    // set instruction limit
    static const auto hook = [](auto L, auto) {
      luaL_error(L, "reached instruction limit (<=1e8)");
    };
    lua_sethook(L, hook, LUA_MASKCOUNT, kSandboxInstructionLimit);

    // set env to empty table
    lua_rawgeti(L, LUA_REGISTRYINDEX, imm_table_);
    lua_setfenv(L, -narg-2);

    // call
    return lua_pcall(L, narg, nret, 0);
  }

  // create and push userdata that holds an instance of T
  // the destructor is guaranteed to be called by GC
  template <typename T, typename... Args>
  static T* NewObj(lua_State* L, Args&&... args) {
    static const auto kName = "Obj_"s+typeid(T).name();

    auto ret = NewObjWithoutMeta<T>(std::forward<Args>(args)...);
    if (luaL_newmetatable(L, kName.c_str())) {
      PushObjDeleter<T>(L);
      lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
    return ret;
  }
  template <typename T, typename... Args>
  static T* NewObjWithoutMeta(lua_State* L, Args&&... args) {
    auto ret = std::make_unique<T>(std::forward<Args>(args)...);
    auto ptr = (T**) lua_newuserdata(L, sizeof(T*));
    return *ptr = ret.release();
  }
  template <typename T>
  static void PushObjDeleter(lua_State* L) {
    static const auto gc = [](auto L) {
      auto udata = *(T**) lua_touserdata(L, 1);
      delete udata;
      return 0;
    };
    lua_pushcfunction(L, gc);
  }
  template <typename T>
  static T* GetObj(lua_State* L, int index, const char* name = nullptr) {
    static const auto kName = "Obj_"s+typeid(T).name();
    if (!name) name = kName.c_str();
    return *(T**) luaL_checkudata(L, index, name);
  }
  template <typename T>
  static T* GetObjIf(lua_State* L, int index, const char* name = nullptr) {
    if (!lua_isuserdata(L, index)) return nullptr;

    auto ptr = lua_touserdata(L, index);
    if (!name) return *(T**) ptr;

    lua_getmetatable(L, index);
    lua_getfield(L, LUA_REGISTRYINDEX, name);
    auto ret = lua_rawequal(L, -1, -2)? *(T**) ptr: nullptr;
    lua_pop(L, 2);
    return ret;
  }

  // Logger object implementation
  struct Logger final {
   public:
    Logger(File*                f = nullptr,
           std::source_location s = std::source_location::current()) noexcept :
        fptr(f), src(s) {
    }
    Logger(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger& operator=(Logger&&) = delete;

    std::mutex mtx;

    File::Path path;
    File*      fptr;

    std::source_location src;
  };
  static void PushLogger(
      lua_State* L, const std::shared_ptr<Logger>& logger) noexcept {
    static const auto f = [](auto L) {
      const auto lv = (notify::Level) lua_tointeger(L, lua_upvalueindex(1));

      auto wlogger = GetObj<std::weak_ptr<Logger>>(L, 1, "Logger");
      auto logger  = wlogger->lock();
      if (!logger) return luaL_error(L, "logger expired");

      auto text = luaL_checkstring(L, 2);

      std::unique_lock<std::mutex> k(logger->mtx);
      notify::Push({logger->src, lv, text, File::Path(logger->path), logger->fptr});
      return 0;
    };
    NewObjWithoutMeta<std::weak_ptr<Logger>>(L, logger);
    if (luaL_newmetatable(L, "Logger")) {
      lua_createtable(L, 0, 0);
        lua_pushinteger(L, static_cast<int>(notify::kInfo));
        lua_pushcclosure(L, f, 1);
        lua_setfield(L, -2, "info");

        lua_pushinteger(L, static_cast<int>(notify::kWarn));
        lua_pushcclosure(L, f, 1);
        lua_setfield(L, -2, "warn");

        lua_pushinteger(L, static_cast<int>(notify::kError));
        lua_pushcclosure(L, f, 1);
        lua_setfield(L, -2, "error");
      lua_setfield(L, -2, "__index");

      PushObjDeleter<std::weak_ptr<Logger>>(L);
      lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
  }

  static void PushValue(lua_State* L, const Value& v) {
    NewObjWithoutMeta<Value>(L, v);
    if (luaL_newmetatable(L, "Value")) {
      lua_createtable(L, 0, 0);
        lua_pushcfunction(L, L_GetValueType);
        lua_setfield(L, -2, "type");

        lua_pushcfunction(L, L_GetValueAs<Value::Integer>);
        lua_setfield(L, -2, "integer");

        lua_pushcfunction(L, L_GetValueAs<Value::Scalar>);
        lua_setfield(L, -2, "scalar");

        lua_pushcfunction(L, L_GetValueAs<Value::String>);
        lua_setfield(L, -2, "string");

        lua_pushcfunction(L, L_GetValueAs<Value::Vec2>);
        lua_setfield(L, -2, "vec2");

        lua_pushcfunction(L, L_GetValueAs<Value::Vec3>);
        lua_setfield(L, -2, "vec3");

        lua_pushcfunction(L, L_GetValueAs<Value::Vec4>);
        lua_setfield(L, -2, "vec4");
      lua_setfield(L, -2, "__index");

      PushObjDeleter<Value>(L);
      lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
  }
  static int L_GetValueType(lua_State* L) {
    auto v = GetObj<Value>(L, 1, "Value");
    lua_pushstring(L, v->StringifyType());
    return 1;
  }
  template <typename T>
  static int L_GetValueAs(lua_State* L) {
    auto v = GetObj<Value>(L, 1, "Value");
    try {
      if constexpr (std::is_same<T, Value::Integer>::value) {
        lua_pushinteger(L, static_cast<Value::Integer>(v->get<Value::Integer>()));
        return 1;

      } else if constexpr (std::is_same<T, Value::Scalar>::value) {
        lua_pushnumber(L, static_cast<Value::Scalar>(v->get<Value::Scalar>()));
        return 1;

      } else if constexpr (std::is_same<T, Value::Boolean>::value) {
        lua_pushboolean(L, v->get<Value::Boolean>());
        return 1;

      } else if constexpr (std::is_same<T, Value::String>::value) {
        const auto& str = v->get<Value::String>();
        lua_pushlstring(L, str.c_str(), str.size());
        return 1;

      } else if constexpr (std::is_same<T, Value::Vec2>::value) {
        const auto& v2 = v->get<Value::Vec2>();
        lua_pushnumber(L, v2[0]);
        lua_pushnumber(L, v2[1]);
        return 2;

      } else if constexpr (std::is_same<T, Value::Vec3>::value) {
        const auto& v3 = v->get<Value::Vec3>();
        lua_pushnumber(L, v3[0]);
        lua_pushnumber(L, v3[1]);
        lua_pushnumber(L, v3[2]);
        return 3;

      } else if constexpr (std::is_same<T, Value::Vec4>::value) {
        const auto& v4 = v->get<Value::Vec4>();
        lua_pushnumber(L, v4[0]);
        lua_pushnumber(L, v4[1]);
        lua_pushnumber(L, v4[2]);
        lua_pushnumber(L, v4[3]);
        return 4;

      } else {
        []<bool f = false>() { static_assert(f, "unknown type"); }();
      }
    } catch (Exception&) {
      lua_pushnil(L);
      return 1;
    }
  }
  template <typename T>
  static int L_PushValue(lua_State* L) {
    if constexpr (std::is_same<T, Value::Pulse>::value) {
      PushValue(L, Value::Pulse());

    } else if constexpr (std::is_same<T, Value::Integer>::value) {
      PushValue(L, static_cast<Value::Integer>(luaL_checkinteger(L, 1)));

    } else if constexpr (std::is_same<T, Value::Scalar>::value) {
      PushValue(L, static_cast<Value::Scalar>(luaL_checknumber(L, 1)));

    } else if constexpr (std::is_same<T, Value::Boolean>::value) {
      PushValue(L, static_cast<Value::Boolean>(!!luaL_checkint(L, 1)));

    } else if constexpr (std::is_same<T, Value::String>::value) {
      PushValue(L, std::string(luaL_checkstring(L, 1)));

    } else if constexpr (std::is_same<T, Value::Vec2>::value) {
      PushValue(L, Value::Vec2(
              luaL_checknumber(L, 1),
              luaL_checknumber(L, 2)));

    } else if constexpr (std::is_same<T, Value::Vec3>::value) {
      PushValue(L, Value::Vec3(
              luaL_checknumber(L, 1),
              luaL_checknumber(L, 2),
              luaL_checknumber(L, 3)));

    } else if constexpr (std::is_same<T, Value::Vec4>::value) {
      PushValue(L, Value::Vec4(
              luaL_checknumber(L, 1),
              luaL_checknumber(L, 2),
              luaL_checknumber(L, 3),
              luaL_checknumber(L, 4)));

    } else {
      []<bool f = false>() { static_assert(f, "unknown type"); }();
    }
    return 1;
  }

 private:
  lua_State* L = nullptr;

  // thread and command queue
  std::thread             th_;
  std::deque<Command>     cmds_;
  std::mutex              mtx_;
  std::condition_variable cv_;

  std::atomic<bool> alive_ = true;


  // lua values (modified only from lua thread)
  int imm_table_ = LUA_REFNIL;


  void Main() noexcept {
    std::unique_lock<std::mutex> k(mtx_);
    while (alive_) {
      cv_.wait(k);
      for (;;) {
        if (cmds_.empty()) break;
        if (!L) {
          k.unlock();
          if (!SetUp()) break;
          k.lock();
        }

        auto cmd = std::move(cmds_.front());
        cmds_.pop_front();

        // clear stack and execute the command
        k.unlock();
        lua_settop(L, 0);
        cmd(L);
        k.lock();
      }
    }
    if (L) lua_close(L);
  }
  bool SetUp() noexcept {
    L = luaL_newstate();
    if (!L) return false;

    // create immutable table for sandboxing
    lua_createtable(L, 0, 0);
      if (luaL_newmetatable(L, "ImmTable")) {
        lua_pushvalue(L, LUA_GLOBALSINDEX);
        lua_setfield(L, -2, "__index");

        lua_pushcfunction(L, [](auto L) { return luaL_error(L, "global is immutable"); });
        lua_setfield(L, -2, "__newindex");
      }
      lua_setmetatable(L, -2);
    imm_table_ = luaL_ref(L, LUA_REGISTRYINDEX);

    // std table
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    lua_createtable(L, 0, 0);
      lua_createtable(L, 0, 0);
        lua_pushcfunction(L, L_PushValue<Value::Pulse>);
        lua_setfield(L, -2, "pulse");

        lua_pushcfunction(L, L_PushValue<Value::Integer>);
        lua_setfield(L, -2, "integer");

        lua_pushcfunction(L, L_PushValue<Value::Scalar>);
        lua_setfield(L, -2, "scalar");

        lua_pushcfunction(L, L_PushValue<Value::Boolean>);
        lua_setfield(L, -2, "boolean");

        lua_pushcfunction(L, L_PushValue<Value::String>);
        lua_setfield(L, -2, "string");

        lua_pushcfunction(L, L_PushValue<Value::Vec2>);
        lua_setfield(L, -2, "vec2");

        lua_pushcfunction(L, L_PushValue<Value::Vec3>);
        lua_setfield(L, -2, "vec3");

        lua_pushcfunction(L, L_PushValue<Value::Vec4>);
        lua_setfield(L, -2, "vec4");
      lua_setfield(L, -2, "value");
    lua_setfield(L, -2, "std");

    return true;
  }
};
LuaJIT dev_;


class Script : public File, public iface::DirItem {
 public:
  static inline TypeInfo type_ = TypeInfo::New<Script>(
      "LuaJIT/Script", "compiled object of LuaJIT script",
      {typeid(iface::DirItem)});


  struct Data final {
   public:
    ~Data() noexcept {
      dev_.Queue([i = reg_func](auto L) { luaL_unref(L, LUA_REGISTRYINDEX, i); });
    }

    std::mutex  mtx;
    std::string msg = "not compiled yet";

    Time lastmod;

    int reg_func = LUA_REFNIL;

    std::atomic<bool> compiling;
  };


  static Script* Cast(const RefStack& ref) {
    auto f = dynamic_cast<Script*>(&*ref);
    if (!f) throw Exception("it's not LuaJIT script");
    return f;
  }


  Script(const std::shared_ptr<Env>& env,
               const std::string& path = "",
               bool shown          = false,
               bool auto_recompile = false) noexcept :
      File(&type_, env), DirItem(kMenu),
      path_(path), shown_(shown), auto_recompile_(auto_recompile) {
    data_ = std::make_shared<Data>();
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    try {
      return std::make_unique<Script>(
          env,
          msgpack::find(obj, "path"s).as<std::string>(),
          msgpack::find(obj, "shown"s).as<bool>(),
          msgpack::find(obj, "auto_recompile"s).as<bool>());
    } catch (msgpack::type_error&) {
      throw DeserializeException("broken LuaJIT/Script");
    }
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(3);

    pk.pack("path"s);
    pk.pack(path_);

    pk.pack("shown"s);
    pk.pack(shown_);

    pk.pack("auto_recompile"s);
    pk.pack(auto_recompile_);
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<Script>(env, path_, shown_, auto_recompile_);
  }

  void Update(RefStack&, Event&) noexcept override;
  void UpdateMenu(RefStack&) noexcept override;
  void UpdateCompiler(RefStack&) noexcept;

  Time lastmod() const noexcept override {
    std::unique_lock<std::mutex> k(data_->mtx);
    return data_->lastmod;
  }
  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::DirItem>(t).Select(this);
  }
  const std::shared_ptr<Data>& data() noexcept {
    return data_;
  }


  void CompileIf(const RefStack& ref) noexcept {
    try {
      auto f = &*ref.Resolve(path_);
      if (f == this) throw Exception("self reference");
      {
        std::unique_lock<std::mutex> k(data_->mtx);
        if (data_->reg_func != LUA_REFNIL && f->lastmod() <= data_->lastmod) {
          return;
        }
      }
      Compile_(ref.GetFullPath(), f);

    } catch (Exception& e) {
      const auto str = "compile failed\n"+e.msg();
      notify::Error(ref, str);

      std::unique_lock<std::mutex> k(data_->mtx);
      data_->msg = std::move(str);
    }
  }
  void Compile(const RefStack& ref) noexcept {
    try {
      Compile_(ref.GetFullPath(), &*ref.Resolve(path_));
    } catch (Exception& e) {
      const auto str = "compile failed\n"+e.msg();
      notify::Error(ref, str);

      std::unique_lock<std::mutex> k(data_->mtx);
      data_->msg = std::move(str);
    }
  }

 private:
  std::shared_ptr<Data> data_;

  // permanentized params
  std::string path_;

  bool shown_          = false;
  bool auto_recompile_ = false;

  // volatile params
  std::string path_editing_;


  void Compile_(File::Path&& selfpath, File* f) {
    // fetch script
    auto factory = File::iface<iface::Factory<Value>>(f);
    if (!factory) throw Exception("no factory interface for Value");
    auto script = factory->Create().get<std::shared_ptr<Value::String>>();

    const auto lastmod = f->lastmod();

    // compile the script
    auto task = [
        self = this,
        path = std::move(selfpath),
        data = data_,
        scr = script,
        lastmod](auto L) mutable {
      data->compiling = true;
      if (luaL_loadstring(L, scr->c_str()) == 0) {
        std::unique_lock<std::mutex> k(data->mtx);
        luaL_unref(L, LUA_REGISTRYINDEX, data->reg_func);

        data->reg_func = luaL_ref(L, LUA_REGISTRYINDEX);
        data->lastmod  = lastmod;
        data->msg      = "ok";
        notify::Trace(path, self, "compile succeeded");
      } else {
        auto str = "compile failed\n"s+luaL_checkstring(L, 1);
        notify::Error(path, self, str);

        std::unique_lock<std::mutex> k(data->mtx);
        data->msg = std::move(str);
      }
      data->compiling = false;
      data = nullptr;
      scr  = nullptr;
    };
    dev_.Queue(std::move(task));
  }
};
void Script::Update(RefStack& ref, Event&) noexcept {
  if (auto_recompile_) CompileIf(ref);

  const auto id = ref.Stringify() + ": LuaJIT Script Compiler";
  if (shown_) {
    if (ImGui::Begin(id.c_str(), &shown_)) {
      if (ImGui::BeginPopupContextWindow()) {
        UpdateMenu(ref);
        ImGui::EndPopup();
      }
      UpdateCompiler(ref);
    }
    ImGui::End();
  }
}
void Script::UpdateMenu(RefStack& ref) noexcept {
  ImGui::MenuItem("Compiler View", nullptr, &shown_);
  ImGui::Separator();

  if (ImGui::MenuItem("Compile")) {
    Compile(ref);
  }
  ImGui::Separator();

  if (ImGui::BeginMenu("script path")) {
    if (gui::InputPathMenu(ref, &path_editing_, &path_)) {
      Compile(ref);
    }
    ImGui::EndMenu();
  }
  ImGui::MenuItem("re-compile automatically", nullptr, &auto_recompile_);
}
void Script::UpdateCompiler(RefStack& ref) noexcept {
  if (ImGui::Button("compile")) Compile(ref);

  std::unique_lock<std::mutex> k(data_->mtx);
  const auto t = Clock::to_time_t(data_->lastmod);
  ImGui::Text("path     : %s", path_.c_str());
  ImGui::Text("lastmod  : %s", std::ctime(&t));
  ImGui::Text("available: %s", data_->reg_func == LUA_REFNIL? "no": "yes");
  ImGui::Text("status   : %s", data_->msg.c_str());
}


class Node : public File, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<Node>(
      "LuaJIT/ScriptNode", "Node driven by LuaJIT/Script",
      {typeid(iface::Node)});

  Node(const std::shared_ptr<Env>& env,
             std::string_view path = "",
             bool auto_rebuild = false,
             const std::vector<std::string>& in  = {},
             const std::vector<std::string>& out = {}) noexcept :
      File(&type_, env), iface::Node(kMenu),
      path_(path), auto_rebuild_(auto_rebuild) {
    life_ = std::make_shared<std::monostate>();

    data_ = std::make_shared<Data>();
    data_->self = this;
    data_->life = life_;

    data_->logger = std::make_shared<LuaJIT::Logger>(
        static_cast<File*>(this), std::source_location::current());

    for (const auto& name : in) {
      in_.push_back(std::make_shared<LuaInSock>(this, name));
    }
    for (const auto& name : out) {
      out_.push_back(std::make_shared<OutSock>(this, name));
    }
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    std::vector<std::string> in, out;
    try {
      auto& socks = msgpack::find(obj, "socks"s);
      in  = msgpack::find(socks, "in"s).as<std::vector<std::string>>();
      out = msgpack::find(socks, "out"s).as<std::vector<std::string>>();
      return std::make_unique<Node>(
          env,
          msgpack::find(obj, "path"s).as<std::string>(),
          msgpack::find(obj, "auto_rebuild"s).as<bool>(),
          in, out);
    } catch (msgpack::type_error&) {
      throw DeserializeException("broken LuaJIT/Node");
    }
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(3);

    pk.pack("path"s);
    pk.pack(path_);

    pk.pack("auto_rebuild"s);
    pk.pack(auto_rebuild_);

    pk.pack("socks"s);
    {
      pk.pack_map(2);

      pk.pack("in"s);
      pk.pack_array(static_cast<uint32_t>(in_.size()));
      for (const auto& in : in_) pk.pack(in->name());

      pk.pack("out"s);
      pk.pack_array(static_cast<uint32_t>(out_.size()));
      for (const auto& out : out_) pk.pack(out->name());
    }
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<Node>(env, path_, auto_rebuild_);
  }

  void Update(RefStack&, Event&) noexcept override;
  void Update(RefStack&, const std::shared_ptr<Context>&) noexcept override;
  void UpdateMenu(RefStack&, const std::shared_ptr<Context>&) noexcept override;

  Time lastmod() const noexcept override {
    std::unique_lock<std::recursive_mutex> k(data_->mtx);
    return data_->lastmod;
  }
  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node>(t).Select(this);
  }

 private:
  struct SockMeta final {
    SockMeta(std::string_view n) noexcept {
      name = n;
    }
    ~SockMeta() noexcept {
      auto task = [reg_handler = reg_handler](auto L) {
        luaL_unref(L, LUA_REGISTRYINDEX, reg_handler);
      };
      dev_.Queue(std::move(task));
    }
    SockMeta(const SockMeta&) = delete;
    SockMeta(SockMeta&&) = delete;
    SockMeta& operator=(const SockMeta&) = delete;
    SockMeta& operator=(SockMeta&&) = delete;

    // immutable params (never be modified after construction)
    std::string name;
    std::string description = "";

    bool cache = true;

    int reg_handler = LUA_REFNIL;
  };
  struct Data final {
    std::recursive_mutex mtx;

    Node* self;
    std::weak_ptr<std::monostate> life;

    std::shared_ptr<LuaJIT::Logger> logger;

    Time lastmod;
    std::string msg = "not built";

    std::vector<std::shared_ptr<SockMeta>> in, out;

    std::atomic<bool> building = false;

    std::atomic<uint8_t> build_try_ = 0;
    std::atomic<uint8_t> build_cnt_ = 0;
    std::atomic<uint8_t> handle_try_   = 0;
    std::atomic<uint8_t> handle_cnt_   = 0;
  };


  std::shared_ptr<std::monostate> life_;
  std::shared_ptr<Data>           data_;

  // permanentized params
  std::string path_;

  bool auto_rebuild_ = true;

  // volatile params
  std::string path_editing_;

  bool force_build_ = true;


  void Build(RefStack& ref) noexcept {
    ++data_->build_try_;

    Script* script;
    {
      std::unique_lock<std::recursive_mutex> k(data_->mtx);
      if (data_->building) return;

      data_->msg = "building...";
      try {
        auto script_ref = ref.Resolve(path_);
        script = Script::Cast(script_ref);
        script->CompileIf(script_ref);
      } catch (Exception& e) {
        auto str = "build failed\n"+e.msg();
        notify::Error(ref, str);
        data_->msg = std::move(str);
        return;
      }
      data_->building = true;
    }

    auto task = [
        self = this,
        path = ref.GetFullPath(),
        data = data_,
        sdata = script->data()](auto L) mutable {
      Time lastmod;
      int  index;
      {
        std::unique_lock<std::mutex> k(sdata->mtx);
        index   = sdata->reg_func;
        lastmod = sdata->lastmod;
      }

      std::vector<std::shared_ptr<SockMeta>> in_bk, out_bk;
      {
        std::unique_lock<std::recursive_mutex> k(data->mtx);
        in_bk  = std::move(data->in);
        out_bk = std::move(data->out);
      }

      lua_rawgeti(L, LUA_REGISTRYINDEX, index);
      try {
        if (dev_.SandboxCall(L, 0, 1) != 0) {
          throw LuaJIT::RuntimeException(lua_tolstring(L, -1, nullptr));
        }
        ApplyBuildResult(L, data);

        std::unique_lock<std::recursive_mutex> k(data->mtx);
        auto task = [self, data = data]() mutable {
          if (!data->life.expired()) self->PostBuild();
          data->building = false;
          data           = nullptr;
        };
        ++data->build_cnt_;

        Queue::sub().Push(std::move(task));
        data->lastmod = lastmod;
        data->msg     = "build succeeded";
        notify::Trace(path, self, "build succeeded");

      } catch (LuaJIT::RuntimeException& e) {
        auto str = "build failed\n"+e.msg();
        notify::Error(path, self, str);

        std::unique_lock<std::recursive_mutex> k(data->mtx);
        data->msg      = std::move(str);
        data->in       = std::move(in_bk);
        data->out      = std::move(out_bk);
        data->building = false;
      }
      data  = nullptr;
      sdata = nullptr;
    };
    dev_.Queue(std::move(task));
  }
  static bool ApplyBuildResult(lua_State* L, const std::shared_ptr<Data>& data) {
    std::unique_lock<std::recursive_mutex> k(data->mtx);

    struct T {
      const char* name;
      std::vector<std::shared_ptr<SockMeta>>* list;
    };
    const auto targets = {
      T {"input", &data->in},
      T {"output", &data->out},
    };
    if (!lua_istable(L, -1)) {
      throw LuaJIT::RuntimeException("expected a Node definition table as a result");
    }
    for (const auto& target : targets) {
      lua_getfield(L, -1, target.name);
      const size_t n = lua_objlen(L, -1);
      for (size_t i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, static_cast<int>(i));
        if (!lua_istable(L, -1)) {
          throw LuaJIT::RuntimeException("input must be an array of pins");
        }

        lua_getfield(L, -1, "name");
        const char* name = lua_tostring(L, -1);
        if (!name || !name[0]) {
          throw LuaJIT::RuntimeException(
              "pin name should be convertible to non-empty string");
        }
        for (auto& sock : *target.list) {
          if (sock->name == name) return false;
        }
        lua_pop(L, 1);

        auto sock = std::make_shared<SockMeta>(name);

        lua_getfield(L, -1, "description");
        const char* desc = lua_tostring(L, -1);
        sock->description = desc? desc: "";
        lua_pop(L, 1);

        lua_getfield(L, -1, "cache");
        sock->cache = lua_toboolean(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, -1, "handler");
        sock->reg_handler = luaL_ref(L, LUA_REGISTRYINDEX);

        target.list->push_back(sock);
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }
    return true;
  }
  void PostBuild() noexcept {
    auto in_bk  = std::move(in_);
    auto out_bk = std::move(out_);

    // sync socks
    std::unique_lock<std::recursive_mutex> k(data_->mtx);
    for (auto& m : data_->in) {
      auto itr = std::find_if(in_bk.begin(), in_bk.end(),
                              [&m](auto& x) { return x->name() == m->name; });
      std::shared_ptr<InSock> sock;
      if (itr != in_bk.end()) {
        auto lsock = std::dynamic_pointer_cast<LuaInSock>(*itr);
        lsock->SwapMeta(m);
        sock = std::move(lsock);
      } else {
        sock = std::make_shared<LuaInSock>(this, m);
      }
      in_.push_back(std::move(sock));
    }
    for (auto& m : data_->out) {
      auto itr = std::find_if(out_bk.begin(), out_bk.end(),
                              [&m](auto& x) { return x->name() == m->name; });
      std::shared_ptr<OutSock> sock;
      if (itr != out_bk.end()) {
        sock = *itr;
      } else {
        sock = std::make_shared<OutSock>(this, m->name);
      }
      out_.push_back(std::move(sock));
    }
  }


  // lua node context for node execution
  class LuaContext : public Node::Context::Data {
   public:
    ~LuaContext() noexcept {
      dev_.Queue([reg = reg_](auto L) { luaL_unref(L, LUA_REGISTRYINDEX, reg); });
    }

    void Push(lua_State* L) {
      if (reg_ == LUA_REFNIL) {
        lua_createtable(L, 0, 0);
        lua_pushvalue(L, -1);
        reg_ = luaL_ref(L, LUA_REGISTRYINDEX);
      } else {
        lua_rawgeti(L, LUA_REGISTRYINDEX, reg_);
      }
    }

   private:
    int reg_ = LUA_REFNIL;
  };

  // InputSocket that calls lua function.
  class LuaInSock : public InSock {
   public:
    LuaInSock(Node* o, std::string_view name) noexcept :
        InSock(o, name), owner_(o), life_(o->life_) {
    }
    LuaInSock(Node* o, const std::shared_ptr<SockMeta>& sock) noexcept :
        InSock(o, sock->name), owner_(o), life_(o->life_), sock_(sock) {
    }

    void SwapMeta(const std::shared_ptr<SockMeta>& sock) noexcept {
      std::unique_lock<std::mutex> k(mtx_);
      sock_ = sock;
    }

    void Receive(const std::shared_ptr<Context>& nctx, Value&& v) noexcept {
      ++owner_->data_->handle_try_;

      std::shared_ptr<SockMeta> sock;
      {
        std::unique_lock<std::mutex> k(mtx_);
        sock = sock_.lock();
      }
      if (!sock || life_.expired()) return;

      if (sock->cache) cache_ = std::move(v);

      auto lctx = nctx->GetOrNew<LuaContext>(owner_);
      auto task = [
          owner = owner_,
          sock,
          data = owner_->data_,
          nctx = nctx,
          lctx,
          v = std::move(v),
          life = life_](auto L) mutable {
        lua_rawgeti(L, LUA_REGISTRYINDEX, sock->reg_handler);
        L_PushEvent(L, v, *lctx, nctx, data);
        if (dev_.SandboxCall(L, 1, 0) == 0) {
          ++data->handle_cnt_;
        } else {
          auto str = "execution failed\n"s+lua_tolstring(L, -1, nullptr);
          {
            std::unique_lock<std::mutex> k(data->logger->mtx);
            notify::Error(data->logger->path, owner, str);
          }
          {
            std::unique_lock<std::recursive_mutex> k(data->mtx);
            data->msg = std::move(str);
          }
        }
        nctx = nullptr;
        lctx = nullptr;
        sock = nullptr;
        data = nullptr;
      };
      dev_.Queue(std::move(task));
    }

   private:
    Node* owner_;

    std::weak_ptr<std::monostate> life_;

    std::mutex mtx_;

    std::weak_ptr<SockMeta> sock_;

    Value cache_;


    // push a table passed to the handler
    struct Event final {
     public:
      Event(const Value& v, const std::weak_ptr<Data>& d, const std::weak_ptr<Context>& n) noexcept :
          value(v), data(d), nctx(n) {
      }
      Value value;
      std::weak_ptr<Data>    data;
      std::weak_ptr<Context> nctx;
    };
    static void L_PushEvent(
        lua_State* L,
        const Value&                    v,
        LuaContext&                     ctx,
        const std::shared_ptr<Context>& nctx,
        const std::shared_ptr<Data>&    data) noexcept {
      dev_.NewObjWithoutMeta<Event>(L, v, data, nctx);
      if (luaL_newmetatable(L, "Node_Event")) {
        lua_createtable(L, 0, 0);
          lua_pushcfunction(L, L_EventValue);
          lua_setfield(L, -2, "value");

          ctx.Push(L);
          lua_setfield(L, -2, "ctx");

          lua_pushcfunction(L, L_EventEmit);
          lua_setfield(L, -2, "emit");

          dev_.PushLogger(L, data->logger);
          lua_setfield(L, -2, "log");
        lua_setfield(L, -2, "__index");

        dev_.PushObjDeleter<Event>(L);
        lua_setfield(L, -2, "__gc");
      }
      lua_setmetatable(L, -2);
    }
    static int L_EventValue(lua_State* L) {
      auto e = dev_.GetObj<Event>(L, 1, "Node_Event");
      dev_.PushValue(L, e->value);
      return 1;
    }
    static int L_EventEmit(lua_State* L) {
      auto e = dev_.GetObj<Event>(L, 1, "Node_Event");

      auto data = e->data.lock();
      auto nctx = e->nctx.lock();
      if (!data) return luaL_error(L, "emitter exipired");
      if (!nctx) return luaL_error(L, "node context exipired");

      const std::string name = luaL_checkstring(L, 2);
      const auto        val  = dev_.GetObj<Value>(L, 3, "Value");

      auto task = [data, nctx, name, val = *val]() mutable {
        if (!data->life.expired()) {
          auto sock = data->self->FindOut(name);
          sock->Send(nctx, std::move(val));
        }
      };
      Queue::sub().Push(std::move(task));
      return 0;
    }
  };
};
void Node::Update(RefStack& ref, Event&) noexcept {
  if (auto_rebuild_ || force_build_) {
    try {
      auto f = &*ref.Resolve(path_);
      if (force_build_ || f->lastmod() > lastmod()) {
        Build(ref);
      }
    } catch (NotFoundException&) {
    }
    force_build_ = false;
  }

  auto& logger = *data_->logger;
  std::unique_lock<std::mutex> k(logger.mtx);
  logger.path = ref.GetFullPath();
}
void Node::Update(RefStack& ref, const std::shared_ptr<Context>& ctx) noexcept {
  ImGui::TextUnformatted("LuaJIT");
  const auto em = ImGui::GetFontSize();

  // keep sapce for status text
  ImGui::SameLine();
  const auto stat_y = ImGui::GetCursorPosY();
  ImGui::NewLine();

  // keep space for path text
  const auto path_y = ImGui::GetCursorPosY();
  ImGui::Dummy({1, ImGui::GetFrameHeight()});

  // display IO pins
  std::unique_lock<std::recursive_mutex> k(data_->mtx);
  ImGui::BeginGroup();
  {
    ImGui::BeginGroup();
    if (data_->in.size() == 0) ImGui::TextDisabled("no input");
    for (auto& sock : data_->in) {
      if (ImNodes::BeginInputSlot(sock->name.c_str(), 1)) {
        gui::NodeSocket();
        ImGui::SameLine();
        ImGui::TextUnformatted(sock->name.c_str());
        ImNodes::EndSlot();
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", sock->description.c_str());
      }
    }
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::Dummy({1*em, 1});
    ImGui::SameLine();

    ImGui::BeginGroup();
    if (data_->out.size() == 0) ImGui::TextDisabled("no output");
    for (auto& sock : data_->out) {
      if (ImNodes::BeginOutputSlot(sock->name.c_str(), 1)) {
        ImGui::TextUnformatted(sock->name.c_str());
        ImGui::SameLine();
        gui::NodeSocket();
        ImNodes::EndSlot();
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", sock->description.c_str());
      }
    }
    ImGui::EndGroup();
  }
  ImGui::EndGroup();
  const auto w = ImGui::GetItemRectSize().x;

  // display msg
  gui::TextCenterChopped(data_->msg, w);

  // display status
  ImGui::SetCursorPosY(stat_y);
  {
    const uint8_t btry = data_->build_try_;
    const uint8_t bcnt = data_->build_cnt_;
    const uint8_t htry = data_->handle_try_;
    const uint8_t hcnt = data_->handle_cnt_;

    char temp[64];
    snprintf(temp, sizeof(temp),
             "%02" PRIX8 "/%02" PRIX8 "-%02" PRIX8 "/%02" PRIX8,
             bcnt, btry, hcnt, htry);

    const auto stat_w = ImGui::CalcTextSize(temp).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+(w-stat_w));
    ImGui::TextDisabled("%s", temp);
  }

  // display current path
  ImGui::SetCursorPosY(path_y);
  ImGui::Button(("-> "s+(path_.empty()? "(empty)"s: path_)).c_str(), {w, 0});
  if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
    UpdateMenu(ref, ctx);
    ImGui::EndPopup();
  }
}
void Node::UpdateMenu(RefStack& ref, const std::shared_ptr<Context>&) noexcept {
  if (ImGui::MenuItem("Rebuild")) Build(ref);
  ImGui::Separator();

  if (ImGui::BeginMenu("script path")) {
    if (gui::InputPathMenu(ref, &path_editing_, &path_)) {
      Build(ref);
    }
    ImGui::EndMenu();
  }
  ImGui::MenuItem("rebuild automatically", nullptr, &auto_rebuild_);
}


class InlineNode final : public File, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<InlineNode>(
      "LuaJIT/InlineNode", "inline Node",
      {typeid(iface::Node)});

  InlineNode(const std::shared_ptr<Env>& env,
             const std::string& expr      = "",
             bool               multiline = false,
             ImVec2             size      = {0.f, 0.f}) noexcept :
      File(&type_, env), Node(kNone),
      data_(std::make_shared<UniversalData>(expr, multiline)), size_(size) {
    out_.emplace_back(new OutSock(this, "out"));

    std::weak_ptr<UniversalData> wdata = data_;
    std::weak_ptr<OutSock>       wout  = out_.back();
    auto task = [self = this, wdata, wout](const auto& ctx, auto&& v) {
      Exec(self, wdata, wout, ctx, std::move(v));
    };
    in_.emplace_back(new LambdaInSock(this, "in", std::move(task)));
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    try {
      return std::make_unique<InlineNode>(
          env,
          msgpack::find(obj, "expr"s).as<std::string>(),
          msgpack::find(obj, "multiline"s).as<bool>(),
          msgpack::as_if<ImVec2>(msgpack::find(obj, "size"s), {0.f, 0.f}));
    } catch (msgpack::type_error&) {
      throw DeserializeException("broken LuaJIT/InlineNode");
    }
  }
  void Serialize(Packer& pk) const noexcept override {
    std::unique_lock<std::mutex> k(data_->mtx);

    pk.pack_map(3);

    pk.pack("expr"s);
    pk.pack(data_->expr);

    pk.pack("multiline"s);
    pk.pack(data_->multiline);

    pk.pack("size");
    pk.pack(size_);
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    std::unique_lock<std::mutex> k(data_->mtx);
    return std::make_unique<InlineNode>(env, data_->expr, data_->multiline, size_);
  }

  void Update(RefStack& ref, Event&) noexcept override {
    data_->path = ref.GetFullPath();
  }
  void Update(RefStack&, const std::shared_ptr<Context>&) noexcept override;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node>(t).Select(this);
  }

 private:
  struct UniversalData final {
   public:
    UniversalData(const std::string& e, bool m) noexcept :
        expr(e), multiline(m) {
    }
    ~UniversalData() noexcept {
      auto task = [reg_func = reg_func](auto L) {
        luaL_unref(L, LUA_REGISTRYINDEX, reg_func);
      };
      dev_.Queue(std::move(task));
    }

    std::mutex mtx;

    std::string expr;
    bool multiline;

    File::Path path;

    bool modified = false;
    int  reg_func = LUA_REFNIL;
  };
  std::shared_ptr<UniversalData> data_;

  ImVec2 size_;


  static std::string WrapCode(const std::string& v, bool multiline) noexcept {
    static const std::string kPrefix = "local v = ...\n";
    if (multiline) {
      return kPrefix+v;
    }
    return multiline? kPrefix+v: kPrefix+"return "+v;
  }
  static void Exec(InlineNode*                         self,
                   const std::weak_ptr<UniversalData>& wdata,
                   const std::weak_ptr<OutSock>&       wout,
                   std::weak_ptr<Context>              wctx,
                   Value&&                             v) noexcept {
    auto task = [self, wdata, wout, wctx, v](auto L) {
      auto data = wdata.lock();
      auto out  = wout.lock();
      auto ctx  = wctx.lock();
      if (!data || !out || !ctx) return;

      {  // preparation
        std::unique_lock<std::mutex> k(data->mtx);
        if (data->modified) {
          luaL_unref(L, LUA_REGISTRYINDEX, data->reg_func);
          data->reg_func = LUA_REFNIL;
          data->modified = false;
        }
        if (data->reg_func == LUA_REFNIL) {
          const auto src = WrapCode(data->expr, data->multiline);
          if (luaL_loadstring(L, src.c_str()) != 0) {
            notify::Error(data->path, self, "compile failed\n"s+lua_tostring(L, -1));
            return;
          }
          data->reg_func = luaL_ref(L, LUA_REGISTRYINDEX);
        }
      }

      // execution
      lua_rawgeti(L, LUA_REGISTRYINDEX, data->reg_func);
      dev_.PushValue(L, v);
      if (dev_.SandboxCall(L, 1, 1) != 0) {
        notify::Error(data->path, self, "execution aborted\n"s+lua_tostring(L, -1));
        return;
      }

      // output result
      const auto ret = dev_.GetObjIf<Value>(L, -1, "Value");
      if (!ret) {
        notify::Warn(data->path, self, "Value is expected, or return nil to do nothing");
        return;
      }
      auto task = [wout, wctx, ret = *ret]() mutable {
        auto out = wout.lock();
        auto ctx = wctx.lock();
        if (out && ctx) out->Send(ctx, std::move(ret));
      };
      Queue::sub().Push(std::move(task));
    };
    dev_.Queue(std::move(task));
  }
};
void InlineNode::Update(RefStack&, const std::shared_ptr<Context>&) noexcept {
  const auto em = ImGui::GetFontSize();
  const auto fh = ImGui::GetFrameHeight();

  ImGui::TextUnformatted("LuaJIT inline");
  if (ImNodes::BeginInputSlot("in", 1)) {
    gui::NodeSocket();
    ImNodes::EndSlot();
  }
  ImGui::SameLine();

  ImGui::BeginGroup();
  {
    std::unique_lock<std::mutex> k(data_->mtx);
    if (data_->multiline) {
      gui::ResizeGroup _("##ResizeGroup", &size_, {8, fh/em}, {16, 8*fh/em}, em);
      if (ImGui::InputTextMultiline("##MultilineExpr", &data_->expr, size_*em)) {
        data_->modified = true;
      }
    } else {
      gui::ResizeGroup _("##ResizeGroup", &size_, {8, fh/em}, {16, fh/em}, em);
      ImGui::SetNextItemWidth(size_.x*em);
      if (ImGui::InputTextWithHint("##SinglelineExpr", "expr", &data_->expr)) {
        data_->modified = true;
      }
    }
    if (ImGui::Checkbox("multiline", &data_->multiline)) {
      data_->expr = "";
    }
  }
  ImGui::EndGroup();

  ImGui::SameLine();
  if (ImNodes::BeginOutputSlot("out", 1)) {
    gui::NodeSocket();
    ImNodes::EndSlot();
  }
}

} }  // namespace kingtaker
