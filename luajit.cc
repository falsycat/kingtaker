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
#include "util/ptr_selector.hh"
#include "util/value.hh"


namespace kingtaker {
namespace {

// LuaJIT device
class LuaJIT final {
 public:
  using Command = std::function<void(lua_State* L)>;


  static constexpr size_t kSandboxInstructionLimit = 10000000;


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
  int SandboxCall(lua_State*, int narg, int nret) noexcept {
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

  template <typename T, typename... Args>
  T* NewObj(lua_State*, Args&&... args) noexcept {
    auto ret = std::make_unique<T>(std::forward<Args>(args)...);
    auto ptr = (T**) lua_newuserdata(L, sizeof(T*));

    static const auto kName = "Obj_"s+typeid(T).name();
    if (luaL_newmetatable(L, kName.c_str())) {
      static const auto gc = [](auto L) {
        auto udata = *(T**) lua_touserdata(L, 1);
        delete udata;
        return 0;
      };
      lua_pushcfunction(L, gc);
      lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
    return *ptr = ret.release();
  }
  template <typename T>
  T* GetObj(lua_State*, int index) noexcept {
    auto ptr = (T**) lua_touserdata(L, index);
    return ptr? *ptr: nullptr;
  }
  template <typename T>
  T& GetObjOrThrow(lua_State*, int index) noexcept {
    auto ret = GetObj<T>(L, index);
    if (!ret) luaL_error(L, "invalid userdata");
    return *ret;
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
      lua_createtable(L, 0, 0);
        lua_createtable(L, 0, 0);
        lua_setfield(L, -2, "__index");

        lua_pushcfunction(L, [](auto L) { return luaL_error(L, "global is immutable"); });
        lua_setfield(L, -2, "__newindex");

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
    std::mutex  mtx;
    std::string msg = "not compiled yet";

    Time lastmod;

    // TODO: unref
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
      data_->msg = e.msg();
    }
  }
  void Compile(File::RefStack& ref) noexcept {
    try {
      Compile_(&*ref.Resolve(path_));
    } catch (Exception& e) {
      std::unique_lock<std::mutex> k(data_->mtx);
      data_->msg = e.msg();
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
             bool auto_rebuild = false) noexcept :
      File(&type_), Node(kMenu),
      path_(path), auto_rebuild_(auto_rebuild) {
    life_ = std::make_shared<std::monostate>();

    data_ = std::make_shared<Data>();
    data_->life = life_;
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj) {
    return std::make_unique<LuaJITNode>(
        msgpack::find(obj, "path"s).as<std::string>(),
        msgpack::find(obj, "auto_rebuild"s).as<bool>());
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(2);

    pk.pack("path"s);
    pk.pack(path_);

    pk.pack("auto_rebuild"s);
    pk.pack(auto_rebuild_);
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
      if (reg_handler == LUA_REFNIL) return;
      auto task = [reg_handler = reg_handler](auto L) {
        luaL_unref(L, LUA_REGISTRYINDEX, reg_handler);
      };
      dev_.Queue(std::move(task));
    }
    SockMeta(const SockMeta&) = delete;
    SockMeta(SockMeta&&) = delete;
    SockMeta& operator=(const SockMeta&) = delete;
    SockMeta& operator=(SockMeta&&) = delete;


    std::string name;
    std::string description = "";

    bool cache = true;

    int reg_handler = LUA_REFNIL;

    std::optional<Value> pending;
  };
  struct Data final {
    std::recursive_mutex mtx;
    std::weak_ptr<std::monostate> life;

    Time lastmod;
    std::string msg = "not built";

    std::vector<std::shared_ptr<SockMeta>> in, out;

    std::atomic<bool> building  = false;
    std::atomic<bool> emittable = false;

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
        data_->msg = e.msg();
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
      const int ret = dev_.SandboxCall(L, 0, 1);
      if (ret == 0 && ApplyBuildResult(L, data)) {
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
      } else {
        std::unique_lock<std::recursive_mutex> k(data->mtx);
        data->msg      = lua_tolstring(L, -1, nullptr);
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
    for (const auto& target : targets) {
      lua_getfield(L, -1, target.name);
      const size_t n = lua_objlen(L, -1);
      for (size_t i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, static_cast<int>(i));
        if (!lua_istable(L, -1)) return false;

        lua_getfield(L, -1, "name");
        const char* name = lua_tostring(L, -1);
        if (!name || !name[0]) return false;
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
    // TODO sync socks
    in_.clear();
    out_.clear();

    std::unique_lock<std::recursive_mutex> k(data_->mtx);
    for (auto& src : data_->in) {
      in_.push_back(std::make_shared<LuaInSock>(this, src));
    }
    for (auto& src : data_->out) {
      if (src->cache) {
        out_.push_back(std::make_shared<CachedOutSock>(this, src->name, Value::Pulse()));
      } else {
        out_.push_back(std::make_shared<OutSock>(this, src->name));
      }
    }
  }


  static void L_PushValue(lua_State* L, const Value& v) noexcept {
    if (v.has<Value::Integer>()) {
      lua_pushinteger(L, v.get<Value::Integer>());
    } else if (v.has<Value::Scalar>()) {
      lua_pushnumber(L, v.get<Value::Scalar>());
    } else {
      lua_pushnil(L);
    }
  }
  static Value L_ToValue(lua_State* L, int n) noexcept {
    if (lua_isnumber(L, n)) {
      return Value::Scalar { lua_tonumber(L, n) };
    } else {
      return Value::Pulse();
    }
  }
  static void L_PushEmitter(lua_State* L, const std::shared_ptr<Data>& data) noexcept {
    std::weak_ptr<Data> wdata = data;
    auto lambda = [](auto* L) {
      auto& wdata = dev_.GetObjOrThrow<std::weak_ptr<Data>>(L, lua_upvalueindex(1));
      auto  data  = wdata.lock();
      if (!data) return luaL_error(L, "emitter exipired");

      std::unique_lock<std::recursive_mutex> k(data->mtx);
      if (!data->emittable) {
        return luaL_error(L, "currently not emittable");
      }

      const auto name = luaL_checkstring(L, 1);
      for (auto& sock : data->out) {
        if (sock->name == name) sock->pending = L_ToValue(L, 2);
      }
      return 0;
    };
    dev_.NewObj<std::weak_ptr<Data>>(L, data);
    lua_pushcclosure(L, lambda, 1);
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
    LuaInSock(LuaJITNode* o, const std::shared_ptr<SockMeta>& sock) noexcept :
        InSock(o, sock->name), owner_(o), life_(o->life_), sock_(sock) {
    }

    void Receive(const std::shared_ptr<Context>& nctx, Value&& v) noexcept {
      ++owner_->data_->handle_try_;

      auto sock = sock_.lock();
      if (!sock || life_.expired()) return;

      if (sock->cache) cache_ = std::move(v);

      auto& lctx = nctx->GetOrNew<LuaContext>(owner_);
      auto  task = [
          sock,
          data = owner_->data_,
          nctx = nctx,
          &lctx,
          v = std::move(v),
          life = life_,
          self = this](auto L) mutable {
        lua_rawgeti(L, LUA_REGISTRYINDEX, sock->reg_handler);

        // push arguments
        L_PushValue(L, v);
        L_PushEmitter(L, data);
        lctx.Push(L);

        // call the handler
        int ret;
        data->emittable = true;
        ret = dev_.SandboxCall(L, 3, 0);
        data->emittable = false;

        // process the result
        if (ret == 0) {
          auto task = [nctx = nctx, life, self]() mutable {
            if (life.lock()) {
              self->PostReceive(nctx);
            }
            nctx = nullptr;
          };
          Queue::sub().Push(std::move(task));
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
    void PostReceive(const std::shared_ptr<Context>& nctx) noexcept {
      auto data = owner_->data_;

      std::unique_lock<std::recursive_mutex> k(data->mtx);
      for (auto& out : data->out) {
        if (!out->pending) continue;

        auto sock = owner_->FindOut(out->name);
        if (sock) sock->Send(nctx, std::move(*out->pending));
        out->pending = std::nullopt;
      }
    }

   private:
    LuaJITNode* owner_;

    std::weak_ptr<std::monostate> life_;

    std::weak_ptr<SockMeta> sock_;

    Value cache_;
  };
};
void LuaJITNode::Update(File::RefStack& ref) noexcept {
  if (auto_rebuild_) {
    try {
      auto f = &*ref.Resolve(path_);
      if (f->lastModified() > lastModified()) Build(ref);
    } catch (NotFoundException&) {
    }
  }
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
  if (data_->msg.size()) {
    const char* msg   = data_->msg.c_str();
    const auto  msg_w = ImGui::CalcTextSize(msg).x;
    if (msg_w < w) {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX()+(w-msg_w)/2);
      ImGui::TextUnformatted(msg);
    } else {
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+w);
      ImGui::TextWrapped("%s", msg);
      ImGui::PopTextWrapPos();
    }
  }

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
