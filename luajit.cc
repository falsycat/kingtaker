#include "kingtaker.hh"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include <lua.hpp>

#include <imgui.h>
#include <imgui_stdlib.h>

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

    int reg_func = LUA_REFNIL;
  };


  static LuaJITScript* Cast(const RefStack& ref) {
    auto f = dynamic_cast<LuaJITScript*>(&*ref);
    if (!f) throw Exception("it's not LuaJIT script");
    return f;
  }


  LuaJITScript(const std::string& path = "",
               bool shown        = false,
               bool auto_compile = false) noexcept :
      File(&type_), DirItem(kMenu),
      path_(path), shown_(shown), auto_compile_(auto_compile) {
    data_ = std::make_shared<Data>();
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj) {
    try {
      return std::make_unique<LuaJITScript>(
          msgpack::find(obj, "path"s).as<std::string>(),
          msgpack::find(obj, "shown"s).as<bool>(),
          msgpack::find(obj, "auto_compile"s).as<bool>());
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

    pk.pack("auto_compile"s);
    pk.pack(auto_compile_);
  }
  std::unique_ptr<File> Clone() const noexcept override {
    return std::make_unique<LuaJITScript>(path_, shown_, auto_compile_);
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

  bool shown_        = false;
  bool auto_compile_ = false;

  // volatile params
  std::string path_editing_;


  void Compile_(File* f) {
    // fetch script
    auto factory = File::iface<iface::Factory<Value>>(f);
    if (!factory) throw Exception("no factory interface for Value");
    auto script = factory->Create().get<std::shared_ptr<Value::String>>();

    // compile the script
    auto task = [data = data_, scr = script](auto L) mutable {
      if (luaL_loadstring(L, scr->c_str()) == 0) {
        std::unique_lock<std::mutex> k(data->mtx);
        luaL_unref(L, LUA_REGISTRYINDEX, data->reg_func);

        data->reg_func = luaL_ref(L, LUA_REGISTRYINDEX);
        data->lastmod  = Clock::now();
        data->msg      = "ok";
      } else {
        std::unique_lock<std::mutex> k(data->mtx);
        data->msg = luaL_checkstring(L, 1);
      }
      data = nullptr;
      scr  = nullptr;
    };
    dev_.Queue(std::move(task));
  }
};
void LuaJITScript::Update(File::RefStack& ref) noexcept {
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

  // TODO(falsycat): auto-recompile
}
void LuaJITScript::UpdateMenu(File::RefStack& ref) noexcept {
  ImGui::MenuItem("Compiler View", nullptr, &shown_);
  ImGui::Separator();

  if (ImGui::MenuItem("Compile")) {
    Compile(ref);
  }
  ImGui::Separator();

  if (ImGui::BeginMenu("script path")) {
    gui::InputPathMenu(ref, &path_editing_, &path_);
    ImGui::EndMenu();
  }
  ImGui::MenuItem("re-compile automatically", nullptr, &auto_compile_);
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


class LuaJITNode : public File, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<LuaJITNode>(
      "LuaJIT Node", "Node driven by LuaJIT",
      {typeid(iface::Node)});

  LuaJITNode() noexcept : File(&type_), Node(kMenu) {
    data_ = std::make_shared<Data>();
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object&) {
    return std::make_unique<LuaJITNode>();
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_nil();
  }
  std::unique_ptr<File> Clone() const noexcept override {
    return std::make_unique<LuaJITNode>();
  }

  void Update(File::RefStack&, const std::shared_ptr<Context>&) noexcept override;
  void UpdateMenu(File::RefStack&, const std::shared_ptr<Context>&) noexcept override;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node>(t).Select(this);
  }

 private:
  struct Data {
    std::mutex  mtx;
    std::string msg;
  };
  std::shared_ptr<Data> data_;

  // permanentized params
  std::string path_;

  bool auto_rebuild_ = true;

  // volatile params
  std::string path_editing_;


  void Rebuild(RefStack& ref) noexcept {
    LuaJITScript* script;
    try {
      auto script_ref = ref.Resolve(path_);
      script = LuaJITScript::Cast(script_ref);
      script->CompileIf(script_ref);
    } catch (Exception&) {
      /* TODO */
      return;
    }

    auto task = [data = data_, sdata = script->data()](auto L) mutable {
      int index;
      {
        std::unique_lock<std::mutex> k(sdata->mtx);
        index = sdata->reg_func;
      }
      lua_rawgeti(L, LUA_REGISTRYINDEX, index);
      if (lua_pcall(L, 0, 0, 0) == 0) {
        // TODO
      } else {
        std::unique_lock<std::mutex> k(data->mtx);
        data->msg = lua_tolstring(L, -1, nullptr);
      }
      data  = nullptr;
      sdata = nullptr;
    };
    dev_.Queue(std::move(task));
  }
};
void LuaJITNode::Update(File::RefStack&, const std::shared_ptr<Context>&) noexcept {
  ImGui::TextUnformatted("LuaJIT Node");

  std::unique_lock<std::mutex> k(data_->mtx);
  ImGui::Text("status: %s", data_->msg.c_str());
}
void LuaJITNode::UpdateMenu(File::RefStack& ref, const std::shared_ptr<Context>&) noexcept {
  if (ImGui::BeginMenu("script path")) {
    if (gui::InputPathMenu(ref, &path_editing_, &path_)) {
      Rebuild(ref);
    }
    ImGui::EndMenu();
  }
  ImGui::MenuItem("auto-rebuild", nullptr, &auto_rebuild_);
}

} }  // namespace kingtaker
