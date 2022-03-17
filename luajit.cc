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
#include <imgui_stdlib.h>
#include <ImNodes.h>

#include "iface/dir.hh"
#include "iface/factory.hh"
#include "iface/gui.hh"
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

  // Logger object implementation
  struct Logger final {
   public:
    Logger(File*                f = nullptr,
           std::source_location s = std::source_location::current()) noexcept :
        fptr(f), src(s) {
    }
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
    auto ret = (T**) luaL_checkudata(L, index, name);
    return ret? *ret: nullptr;
  }

  // Value type conversion
  static void PushValue(lua_State* L, const Value& v) {
    if (v.has<Value::Pulse>()) {
      lua_pushnil(L);

    } else if (v.has<Value::Integer>()) {
      lua_pushinteger(L, v.get<Value::Integer>());

    } else if (v.has<Value::Scalar>()) {
      lua_pushnumber(L, v.get<Value::Scalar>());

    } else if (v.has<Value::Boolean>()) {
      lua_pushboolean(L, v.get<Value::Boolean>());

    } else if (v.has<Value::String>()) {
      const auto& s = v.get<Value::String>();
      lua_pushlstring(L, s.data(), s.size());

    } else if (v.has<Value::Vec2>()) {
      const auto& vec = v.get<Value::Vec2>();
      lua_createtable(L, 2, 0);
        lua_pushnumber(L, vec.x);
        lua_rawseti(L, -2, 1);
        lua_pushnumber(L, vec.y);
        lua_rawseti(L, -2, 2);

    } else if (v.has<Value::Vec3>()) {
      const auto& vec = v.get<Value::Vec3>();
      lua_createtable(L, 3, 0);
        lua_pushnumber(L, vec.x);
        lua_rawseti(L, -2, 1);
        lua_pushnumber(L, vec.y);
        lua_rawseti(L, -2, 2);
        lua_pushnumber(L, vec.z);
        lua_rawseti(L, -2, 3);

    } else if (v.has<Value::Vec4>()) {
      const auto& vec = v.get<Value::Vec3>();
      lua_createtable(L, 3, 0);
        lua_pushnumber(L, vec[0]);
        lua_rawseti(L, -2, 1);
        lua_pushnumber(L, vec[1]);
        lua_rawseti(L, -2, 2);
        lua_pushnumber(L, vec[2]);
        lua_rawseti(L, -2, 3);
        lua_pushnumber(L, vec[3]);
        lua_rawseti(L, -2, 4);

    } else {
      lua_pushnil(L);
    }
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
        lua_createtable(L, 0, 0);
        lua_setfield(L, -2, "__index");

        lua_pushcfunction(L, [](auto L) { return luaL_error(L, "global is immutable"); });
        lua_setfield(L, -2, "__newindex");
      }
      lua_setmetatable(L, -2);
    imm_table_ = luaL_ref(L, LUA_REGISTRYINDEX);

    return true;
  }
};
LuaJIT dev_;


class LuaJITScript : public File, public iface::GUI, public iface::DirItem {
 public:
  static inline TypeInfo type_ = TypeInfo::New<LuaJITScript>(
      "LuaJIT Script", "LuaJIT script",
      {typeid(iface::DirItem), typeid(iface::GUI)});


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


  static LuaJITScript* Cast(const RefStack& ref) {
    auto f = dynamic_cast<LuaJITScript*>(&*ref);
    if (!f) throw Exception("it's not LuaJIT script");
    return f;
  }


  LuaJITScript(const std::string& path = "",
               bool shown          = false,
               bool auto_recompile = false) noexcept :
      File(&type_), DirItem(kMenu),
      path_(path), shown_(shown), auto_recompile_(auto_recompile) {
    data_ = std::make_shared<Data>();
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj) {
    try {
      return std::make_unique<LuaJITScript>(
          msgpack::find(obj, "path"s).as<std::string>(),
          msgpack::find(obj, "shown"s).as<bool>(),
          msgpack::find(obj, "auto_recompile"s).as<bool>());
    } catch (msgpack::type_error&) {
      throw DeserializeException("broken LuaJIT Script");
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
  std::unique_ptr<File> Clone() const noexcept override {
    return std::make_unique<LuaJITScript>(path_, shown_, auto_recompile_);
  }

  void Update(File::RefStack&) noexcept override;
  void UpdateMenu(File::RefStack&) noexcept override;
  void UpdateCompiler(File::RefStack&) noexcept;

  Time lastModified() const noexcept override {
    std::unique_lock<std::mutex> k(data_->mtx);
    return data_->lastmod;
  }
  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::GUI, iface::DirItem>(t).Select(this);
  }
  const std::shared_ptr<Data>& data() noexcept {
    return data_;
  }


  void CompileIf(const File::RefStack& ref) noexcept {
    try {
      auto f = &*ref.Resolve(path_);
      if (f == this) throw Exception("self reference");
      {
        std::unique_lock<std::mutex> k(data_->mtx);
        if (data_->reg_func != LUA_REFNIL && f->lastModified() <= data_->lastmod) {
          return;
        }
      }
      Compile_(f);

    } catch (Exception& e) {
      std::unique_lock<std::mutex> k(data_->mtx);
      data_->msg = "compile failed\n"+e.msg();
    }
  }
  void Compile(File::RefStack& ref) noexcept {
    try {
      Compile_(&*ref.Resolve(path_));
    } catch (Exception& e) {
      std::unique_lock<std::mutex> k(data_->mtx);
      data_->msg = "compile failed\n"+e.msg();
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


  void Compile_(File* f) {
    // fetch script
    auto factory = File::iface<iface::Factory<Value>>(f);
    if (!factory) throw Exception("no factory interface for Value");
    auto script = factory->Create().get<std::shared_ptr<Value::String>>();

    const auto lastmod = f->lastModified();

    // compile the script
    auto task = [data = data_, scr = script, lastmod](auto L) mutable {
      data->compiling = true;
      if (luaL_loadstring(L, scr->c_str()) == 0) {
        std::unique_lock<std::mutex> k(data->mtx);
        luaL_unref(L, LUA_REGISTRYINDEX, data->reg_func);

        data->reg_func = luaL_ref(L, LUA_REGISTRYINDEX);
        data->lastmod  = lastmod;
        data->msg      = "ok";
      } else {
        std::unique_lock<std::mutex> k(data->mtx);
        data->msg = luaL_checkstring(L, 1);
      }
      data->compiling = false;
      data = nullptr;
      scr  = nullptr;
    };
    dev_.Queue(std::move(task));
  }
};
void LuaJITScript::Update(File::RefStack& ref) noexcept {
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
void LuaJITScript::UpdateMenu(File::RefStack& ref) noexcept {
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
void LuaJITScript::UpdateCompiler(File::RefStack& ref) noexcept {
  if (ImGui::Button("compile")) Compile(ref);

  std::unique_lock<std::mutex> k(data_->mtx);
  const auto t = Clock::to_time_t(data_->lastmod);
  ImGui::Text("path     : %s", path_.c_str());
  ImGui::Text("lastmod  : %s", std::ctime(&t));
  ImGui::Text("available: %s", data_->reg_func == LUA_REFNIL? "no": "yes");
  ImGui::Text("status   : %s", data_->msg.c_str());
}


class LuaJITNode : public File, public iface::GUI, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<LuaJITNode>(
      "LuaJIT Node", "Node driven by LuaJIT",
      {typeid(iface::GUI), typeid(iface::Node)});

  LuaJITNode(std::string_view path = "",
             bool auto_rebuild = false,
             const std::vector<std::string>& in  = {},
             const std::vector<std::string>& out = {}) noexcept :
      File(&type_), Node(kMenu),
      path_(path), auto_rebuild_(auto_rebuild) {
    life_ = std::make_shared<std::monostate>();

    data_ = std::make_shared<Data>();
    data_->self = this;
    data_->life = life_;

    data_->logger = std::make_shared<LuaJIT::Logger>(this);

    for (const auto& name : in) {
      in_.push_back(std::make_shared<LuaInSock>(this, name));
    }
    for (const auto& name : out) {
      out_.push_back(std::make_shared<OutSock>(this, name));
    }
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj) {
    std::vector<std::string> in, out;
    try {
      in  = msgpack::find(msgpack::find(
              obj, "socks"s), "in"s).as<std::vector<std::string>>();
      out = msgpack::find(msgpack::find(
              obj, "socks"s), "out"s).as<std::vector<std::string>>();
    } catch (msgpack::type_error&) {
    }
    return std::make_unique<LuaJITNode>(
        msgpack::find(obj, "path"s).as<std::string>(),
        msgpack::find(obj, "auto_rebuild"s).as<bool>(),
        in, out);
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
  std::unique_ptr<File> Clone() const noexcept override {
    return std::make_unique<LuaJITNode>(path_, auto_rebuild_);
  }

  void Update(File::RefStack&) noexcept override;
  void Update(File::RefStack&, const std::shared_ptr<Context>&) noexcept override;
  void UpdateMenu(File::RefStack&, const std::shared_ptr<Context>&) noexcept override;

  Time lastModified() const noexcept override {
    std::unique_lock<std::recursive_mutex> k(data_->mtx);
    return data_->lastmod;
  }
  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::GUI, iface::Node>(t).Select(this);
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

    LuaJITNode* self;
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

    LuaJITScript* script;
    {
      std::unique_lock<std::recursive_mutex> k(data_->mtx);
      if (data_->building) return;

      data_->msg = "building...";
      try {
        auto script_ref = ref.Resolve(path_);
        script = LuaJITScript::Cast(script_ref);
        script->CompileIf(script_ref);
      } catch (Exception& e) {
        data_->msg = "build failed\n"+e.msg();
        return;
      }
      data_->building = true;
    }

    auto task = [self = this, data = data_, sdata = script->data()](auto L) mutable {
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
          if (data->life.lock()) self->PostBuild();
          data->building = false;
          data           = nullptr;
        };
        Queue::sub().Push(std::move(task));
        data->msg     = "build succeeded";
        data->lastmod = lastmod;
        ++data->build_cnt_;

      } catch (LuaJIT::RuntimeException& e) {
        std::unique_lock<std::recursive_mutex> k(data->mtx);
        data->msg      = "build failed\n"+e.msg();
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
    LuaInSock(LuaJITNode* o, std::string_view name) noexcept :
        InSock(o, name), owner_(o), life_(o->life_) {
    }
    LuaInSock(LuaJITNode* o, const std::shared_ptr<SockMeta>& sock) noexcept :
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

      auto& lctx = nctx->GetOrNew<LuaContext>(owner_);
      auto  task = [
          sock,
          data = owner_->data_,
          nctx = nctx,
          &lctx,
          v = std::move(v),
          life = life_](auto L) mutable {
        lua_rawgeti(L, LUA_REGISTRYINDEX, sock->reg_handler);
        L_PushEvent(L, v, lctx, nctx, data);
        if (dev_.SandboxCall(L, 1, 0) == 0) {
          ++data->handle_cnt_;
        } else {
          data->msg = lua_tolstring(L, -1, nullptr);
        }
        nctx = nullptr;
        sock = nullptr;
        data = nullptr;
      };
      dev_.Queue(std::move(task));
    }

   private:
    LuaJITNode* owner_;

    std::weak_ptr<std::monostate> life_;

    std::mutex mtx_;

    std::weak_ptr<SockMeta> sock_;

    Value cache_;


    // push a table passed to the handler
    static void L_PushEvent(
        lua_State* L,
        const Value&                    v,
        LuaContext&                     ctx,
        const std::shared_ptr<Context>& nctx,
        const std::shared_ptr<Data>&    data) noexcept {
      lua_createtable(L, 0, 4);
        dev_.PushValue(L, v);
        lua_setfield(L, -2, "value");

        ctx.Push(L);
        lua_setfield(L, -2, "ctx");

        L_PushEmitter(L, data, nctx);
        lua_setfield(L, -2, "emit");

        dev_.PushLogger(L, data->logger);
        lua_setfield(L, -2, "log");
    }

    // emit function implementation
    struct Emitter final {
      std::weak_ptr<Data>    data;
      std::weak_ptr<Context> nctx;
    };
    static void L_PushEmitter(
        lua_State* L,
        const std::shared_ptr<Data>&    data,
        const std::shared_ptr<Context>& nctx) noexcept {
      dev_.NewObjWithoutMeta<Emitter>(L, Emitter {data, nctx});
      if (luaL_newmetatable(L, "Node_Emitter")) {
        lua_createtable(L, 0, 16);
          lua_pushcfunction(L, L_Emitter<Value::Pulse>);
          lua_setfield(L, -2, "pulse");

          lua_pushcfunction(L, L_Emitter<Value::Integer>);
          lua_setfield(L, -2, "integer");

          lua_pushcfunction(L, L_Emitter<Value::Scalar>);
          lua_setfield(L, -2, "scalar");

          lua_pushcfunction(L, L_Emitter<Value::Boolean>);
          lua_setfield(L, -2, "boolean");

          lua_pushcfunction(L, L_Emitter<Value::String>);
          lua_setfield(L, -2, "string");

          lua_pushcfunction(L, L_Emitter<Value::Vec2>);
          lua_setfield(L, -2, "vec2");

          lua_pushcfunction(L, L_Emitter<Value::Vec3>);
          lua_setfield(L, -2, "vec3");

          lua_pushcfunction(L, L_Emitter<Value::Vec4>);
          lua_setfield(L, -2, "vec4");
        lua_setfield(L, -2, "__index");

        dev_.PushObjDeleter<Emitter>(L);
        lua_setfield(L, -2, "__gc");
      }
      lua_setmetatable(L, -2);
    }
    template <typename T>
    static int L_Emitter(lua_State* L) {
      auto emitter = dev_.GetObj<Emitter>(L, 1, "Node_Emitter");

      auto data = emitter->data.lock();
      auto nctx = emitter->nctx.lock();
      if (!data) return luaL_error(L, "emitter exipired");
      if (!nctx) return luaL_error(L, "node context exipired");

      const std::string name = luaL_checkstring(L, 2);

      constexpr int kValIndex = 3;
      Value ret;
      if constexpr (std::is_same<T, Value::Pulse>::value) {
        ret = Value::Pulse {};
      } else if constexpr (std::is_same<T, Value::Integer>::value) {
        ret = Value::Integer {lua_tointeger(L, kValIndex)};
      } else if constexpr (std::is_same<T, Value::Scalar>::value) {
        ret = Value::Scalar {lua_tonumber(L, kValIndex)};
      } else if constexpr (std::is_same<T, Value::Boolean>::value) {
        ret = Value::Boolean {!!lua_toboolean(L, kValIndex)};
      } else if constexpr (std::is_same<T, Value::String>::value) {
        ret = Value::String {lua_tostring(L, kValIndex)};
      } else if constexpr (std::is_same<T, Value::Vec2>::value) {
        luaL_checktype(L, kValIndex, LUA_TTABLE);
        lua_rawgeti(L, kValIndex, 1);
        lua_rawgeti(L, kValIndex, 2);
        ret = Value::Vec2 {
          luaL_checknumber(L, -2),
          luaL_checknumber(L, -1)
        };
      } else if constexpr (std::is_same<T, Value::Vec3>::value) {
        luaL_checktype(L, kValIndex, LUA_TTABLE);
        lua_rawgeti(L, kValIndex, 1);
        lua_rawgeti(L, kValIndex, 2);
        lua_rawgeti(L, kValIndex, 3);
        ret = Value::Vec3 {
          luaL_checknumber(L, -3),
          luaL_checknumber(L, -2),
          luaL_checknumber(L, -1)
        };
      } else if constexpr (std::is_same<T, Value::Vec4>::value) {
        luaL_checktype(L, kValIndex, LUA_TTABLE);
        lua_rawgeti(L, kValIndex, 1);
        lua_rawgeti(L, kValIndex, 2);
        lua_rawgeti(L, kValIndex, 3);
        lua_rawgeti(L, kValIndex, 4);
        ret = Value::Vec4 {
          luaL_checknumber(L, -4),
          luaL_checknumber(L, -3),
          luaL_checknumber(L, -2),
          luaL_checknumber(L, -1)
        };
      } else {
        luaL_error(L, "unknown type");
      }
      auto task = [data, nctx, name, value = std::move(ret)]() mutable {
        if (data->life.lock()) {
          auto sock = data->self->FindOut(name);
          sock->Send(nctx, std::move(value));
        }
        data = nullptr;
        nctx = nullptr;
      };
      Queue::sub().Push(std::move(task));
      return 0;
    }
  };
};
void LuaJITNode::Update(File::RefStack& ref) noexcept {
  if (auto_rebuild_ || force_build_) {
    try {
      auto f = &*ref.Resolve(path_);
      if (force_build_ || f->lastModified() > lastModified()) {
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
void LuaJITNode::Update(File::RefStack& ref, const std::shared_ptr<Context>& ctx) noexcept {
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
void LuaJITNode::UpdateMenu(File::RefStack& ref, const std::shared_ptr<Context>&) noexcept {
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

} }  // namespace kingtaker
