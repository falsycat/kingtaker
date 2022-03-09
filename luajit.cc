#include "kingtaker.hh"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

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


  LuaJIT() noexcept {
    th_ = std::thread([this]() { Main(); });
  }
  ~LuaJIT() noexcept {
    alive_ = false;
    cv_.notify_all();
    th_.join();
  }

  void Queue(Command&& cmd) noexcept {
    cmds_.push_back(std::move(cmd));
    cv_.notify_all();
  }

 private:
  lua_State* L = nullptr;

  // thread and command queue
  std::thread             th_;
  std::deque<Command>     cmds_;
  std::mutex              mtx_;
  std::condition_variable cv_;

  std::atomic<bool> alive_ = true;


  void Main() noexcept {
    while (alive_) {
      {
        std::unique_lock<std::mutex> k(mtx_);
        cv_.wait(k);
      }
      for (;;) {
        std::unique_lock<std::mutex> k(mtx_);
        if (cmds_.empty()) break;
        if (!L) L = luaL_newstate();
        if (!L) break;
        auto cmd = std::move(cmds_.front());
        cmds_.pop_front();
        k.unlock();
        cmd(L);
      }
    }
    if (L) lua_close(L);
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
  struct Sock final {
    std::string name;
    std::string description = "";

    bool cache = true;

    int reg_handler = LUA_REFNIL;

    Sock(std::string_view n) noexcept {
      name = n;
    }
    ~Sock() noexcept {
      if (reg_handler == LUA_REFNIL) return;
      auto task = [reg_handler = reg_handler](auto L) {
        luaL_unref(L, LUA_REGISTRYINDEX, reg_handler);
      };
      dev_.Queue(std::move(task));
    }
    Sock(const Sock&) = delete;
    Sock(Sock&&) = delete;
    Sock& operator=(const Sock&) = delete;
    Sock& operator=(Sock&&) = delete;
  };
  struct Data final {
    std::recursive_mutex mtx;
    std::weak_ptr<std::monostate> life;

    Time lastmod;
    std::string msg = "not built";

    std::vector<std::unique_ptr<Sock>> in, out;

    std::atomic<bool> building = false;
    std::atomic<bool> setup    = false;
  };
  std::shared_ptr<std::monostate> life_;
  std::shared_ptr<Data>           data_;

  // permanentized params
  std::string path_;

  bool auto_rebuild_ = true;

  // volatile params
  std::string path_editing_;


  static void CreateEnvTable(lua_State* L, const std::shared_ptr<Data>& data) {
    lua_createtable(L, 0, 0);  // G
      lua_pushlightuserdata(L, data.get());
      lua_pushlightuserdata(L, &data->in);
      lua_pushcclosure(L, L_Sock_new, 2);
      lua_setfield(L, -2, "Input");

      lua_pushlightuserdata(L, data.get());
      lua_pushlightuserdata(L, &data->out);
      lua_pushcclosure(L, L_Sock_new, 2);
      lua_setfield(L, -2, "Output");
  }
  static int L_Sock_new(lua_State* L) {
    auto data = (Data*) lua_touserdata(L, lua_upvalueindex(1));
    // data is locked while setup phase

    if (!data->setup) {
      return luaL_error(L, "adding socket can be called while setup phase");
    }

    const auto name = luaL_checkstring(L, 1);
    auto       sock = std::make_unique<Sock>(name);

    lua_createtable(L, 0, 0);
      lua_pushlightuserdata(L, data);
      lua_pushlightuserdata(L, sock.get());
      lua_pushcclosure(L, L_Sock_set, 2);
      lua_setfield(L, -2, "set");

    auto socks = (std::vector<std::unique_ptr<Sock>>*) lua_touserdata(L, lua_upvalueindex(2));
    socks->push_back(std::move(sock));
    return 1;
  }
  static int L_Sock_set(lua_State* L) {
    auto data = (Data*) lua_touserdata(L, lua_upvalueindex(1));
    // data is locked while setup phase

    if (!data->setup) {
      return luaL_error(L, "adding socket can be called while setup phase");
    }

    auto        sock = (Sock*) lua_touserdata(L, lua_upvalueindex(2));
    const char* name = luaL_checkstring(L, 2);

    static constexpr auto kValueIndex = 3;
    if (0 == std::strcmp(name, "description")) {
      sock->description = luaL_checkstring(L, kValueIndex);

    } else if (0 == std::strcmp(name, "cache")) {
      sock->cache = !!luaL_checkint(L, kValueIndex);

    } else if (0 == std::strcmp(name, "handler")) {
      luaL_checktype(L, kValueIndex, LUA_TFUNCTION);
      lua_pushvalue(L, kValueIndex);
      sock->reg_handler = luaL_ref(L, LUA_REGISTRYINDEX);

    } else {
      return luaL_error(L, "unknown field '%s'", name);
    }
    lua_pushvalue(L, 1);
    return 1;
  }


  void Build(RefStack& ref) noexcept {
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
    }

    auto task = [self = this, data = data_, sdata = script->data()](auto L) mutable {
      data->building = true;
      {
        Time lastmod;
        int  index;
        {
          std::unique_lock<std::mutex> k(sdata->mtx);
          index   = sdata->reg_func;
          lastmod = sdata->lastmod;
        }

        std::unique_lock<std::recursive_mutex> k(data->mtx);

        auto in_bk  = std::move(data->in);
        auto out_bk = std::move(data->out);

        lua_rawgeti(L, LUA_REGISTRYINDEX, index);
        CreateEnvTable(L, data);
        lua_setfenv(L, -2);

        data->setup = true;
        if (lua_pcall(L, 0, 0, 0) == 0) {
          auto task = [self, data = data]() mutable {
            if (data->life.lock()) self->BuildPostproc();
            data->building = false;
            data           = nullptr;
          };
          Queue::main().Push(std::move(task));
          data->msg     = "build succeeded";
          data->lastmod = lastmod;
        } else {
          data->msg      = lua_tolstring(L, -1, nullptr);
          data->in       = std::move(in_bk);
          data->out      = std::move(out_bk);
          data->building = false;
        }
        data->setup = false;
      }
      data  = nullptr;
      sdata = nullptr;
    };
    dev_.Queue(std::move(task));
  }
  void BuildPostproc() noexcept {
    // TODO: use custom socket to receive events
    std::unique_lock<std::recursive_mutex> k(data_->mtx);
    for (auto& src : data_->in) {
      if (src->cache) {
        in_.push_back(std::make_shared<CachedInSock>(this, src->name, Value::Pulse()));
      } else {
        in_.push_back(std::make_shared<InSock>(this, src->name));
      }
    }
    for (auto& src : data_->out) {
      if (src->cache) {
        out_.push_back(std::make_shared<CachedOutSock>(this, src->name, Value::Pulse()));
      } else {
        out_.push_back(std::make_shared<OutSock>(this, src->name));
      }
    }
  }
};
void LuaJITNode::Update(File::RefStack& ref) noexcept {
  if (auto_rebuild_) {
    try {
      auto f = &*ref.Resolve(path_);
      if (f->lastModified() > lastModified()) Build(ref);
    } catch (NotFoundException& e) {
    }
  }
}
void LuaJITNode::Update(File::RefStack& ref, const std::shared_ptr<Context>& ctx) noexcept {
  ImGui::TextUnformatted("LuaJIT Node");

  const auto em = ImGui::GetFontSize();

  const auto top = ImGui::GetCursorPosY();
  ImGui::Dummy({1, ImGui::GetFrameHeight()});

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
        ImGui::SetTooltip(sock->description.c_str());
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
        ImGui::SetTooltip(sock->description.c_str());
      }
    }
    ImGui::EndGroup();
  }
  ImGui::EndGroup();
  const auto w = ImGui::GetItemRectSize().x;

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

  ImGui::SetCursorPosY(top);
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
