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

// LuaJIT context file which owns lua_State*
class LuaJIT : public File, public iface::GUI, public iface::DirItem {
 public:
  static inline TypeInfo type_ = TypeInfo::New<LuaJIT>(
      "LuaJIT", "LuaJIT module",
      {typeid(iface::DirItem), typeid(iface::GUI)});


  using Command = std::function<void(lua_State* L)>;


  static LuaJIT* Find(const RefStack& ref, const std::string& path) {
    auto ctx = dynamic_cast<LuaJIT*>(&*ref.Resolve(path));
    if (!ctx) throw Exception("it's not LuaJIT context: "s+path);
    return ctx;
  }


  LuaJIT() noexcept : File(&type_), DirItem(kMenu) {
    th_ = std::thread([this]() { Main(); });
  }
  ~LuaJIT() noexcept {
    alive_ = false;
    cv_.notify_all();
    th_.join();
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object&) {
    return std::make_unique<LuaJIT>();
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_nil();
  }
  std::unique_ptr<File> Clone() const noexcept override {
    return std::make_unique<LuaJIT>();
  }

  void Queue(Command&& cmd) noexcept {
    cmds_.push_back(std::move(cmd));
    cv_.notify_all();
  }

  void Update(RefStack& ref) noexcept override;
  void UpdateMenu(RefStack&) noexcept override;
  void UpdateInspector(RefStack&) noexcept;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::DirItem, iface::GUI>(t).Select(this);
  }

 private:
  lua_State* L = nullptr;

  // thread and command queue
  std::thread             th_;
  std::deque<Command>     cmds_;
  std::mutex              mtx_;
  std::condition_variable cv_;

  std::atomic<bool> init_  = true;
  std::atomic<bool> alive_ = true;
  std::atomic<bool> dead_  = false;

  // permanentized parameters
  bool shown_ = false;

  // volatile parameters for GUI
  std::string inline_expr_;
  std::string inline_result_;

  std::mutex               logs_mtx_;
  std::vector<std::string> logs_;


  void Main() noexcept {
    L = luaL_newstate();
    init_ = false;
    if (L) {
      SetupBuiltinFeatures();
      while (alive_) {
        std::unique_lock<std::mutex> k(mtx_);
        cv_.wait(k);
        k.unlock();
        for (;;) {
          k.lock();
          if (cmds_.empty()) break;
          auto cmd = std::move(cmds_.front());
          cmds_.pop_front();
          k.unlock();
          cmd(L);
        }
      }
      lua_close(L);
    }
    dead_ = true;
  }


  void SetupBuiltinFeatures() noexcept {
    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, L_debug, 1);
    lua_setglobal(L, "debug");
  }
  static int L_debug(lua_State* L) {
    auto f = (LuaJIT*) lua_topointer(L, lua_upvalueindex(1));
    std::unique_lock<std::mutex> k(f->logs_mtx_);
    f->logs_.push_back(luaL_checkstring(L, 1));
    return 0;
  }
};
void LuaJIT::Update(RefStack& ref) noexcept {
  const auto id = ref.Stringify() + ": LuaJIT Context Inspector";
  if (shown_) {
    if (ImGui::Begin(id.c_str())) {
      UpdateInspector(ref);
    }
    ImGui::End();
  }
}
void LuaJIT::UpdateMenu(RefStack&) noexcept {
  ImGui::MenuItem("Context Inspector", nullptr, &shown_);
}
void LuaJIT::UpdateInspector(RefStack&) noexcept {
  constexpr auto kHeaderFlags = ImGuiTreeNodeFlags_DefaultOpen;

  const auto& style = ImGui::GetStyle();

  if (init_) {
    ImGui::Text("initializing context...");
    ImGui::Text("be patient :)");
    return;
  }
  if (!L) {
    ImGui::Text("context creation failed!!");
    ImGui::Text("memory shortage?");
    return;
  }

  ImGui::Text("LuaJIT version: %.0f", *lua_version(L));

  if (ImGui::CollapsingHeader("Inline Execution", kHeaderFlags)) {
    constexpr auto kHint  = "enter to execute...";
    constexpr auto kFlags = ImGuiInputTextFlags_EnterReturnsTrue;

    ImGui::SetNextItemWidth(ImGui::CalcTextSize("expr").x + style.ItemInnerSpacing.x);
    ImGui::LabelText("##InlineExprLabel", "expr");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::InputTextWithHint("##InlineExpr", kHint, &inline_expr_, kFlags)) {
      ImGui::SetKeyboardFocusHere(-1);

      // Queue a task that executes inline expr and assigns its result into
      // inline_result_.
      Queue([this, expr = inline_expr_](auto L) noexcept {
        luaL_dostring(L, ("return "s + expr).c_str());

        std::string ret;
        if (lua_isstring(L, -1)) {
          ret = lua_tostring(L, -1);
        } else {
          ret = lua_typename(L, lua_type(L, -1));
        }
        // FIXME: sync issue (this file may be removed when this task is executed)
        Queue::main().Push([this, ret]() { inline_result_ = ret; });
      });
    }
    if (inline_result_.size()) {
      ImGui::TextWrapped("%s", inline_result_.c_str());
    }
  }

  if (ImGui::CollapsingHeader("Debug Output", kHeaderFlags)) {
    constexpr auto kFlags = ImGuiWindowFlags_HorizontalScrollbar;
    if (ImGui::BeginChild("scroll", {0, 0}, true, kFlags)) {
      ImGuiListClipper clip;
      clip.Begin(static_cast<int>(logs_.size()));
      for (const auto& v : logs_) {
        ImGui::TextUnformatted(v.c_str());
      }
      clip.End();

      if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.f);
      }
    }
    ImGui::EndChild();
  }
}


class LuaJITScript : public File, public iface::GUI, public iface::DirItem {
 public:
  static inline TypeInfo type_ = TypeInfo::New<LuaJITScript>(
      "LuaJIT Script", "LuaJIT script",
      {typeid(iface::DirItem), typeid(iface::GUI)});

  LuaJITScript(const std::string& ctx  = "",
               const std::string& path = "",
               bool shown        = false,
               bool auto_compile = false) noexcept :
      File(&type_), DirItem(kMenu),
      path_ctx_(ctx), path_(path), shown_(shown), auto_compile_(auto_compile) {
    data_ = std::make_shared<Data>();
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj) {
    try {
      return std::make_unique<LuaJITScript>(
          msgpack::find(obj, "ctx"s).as<std::string>(),
          msgpack::find(obj, "path"s).as<std::string>(),
          msgpack::find(obj, "shown"s).as<bool>(),
          msgpack::find(obj, "auto_compile"s).as<bool>());
    } catch (msgpack::type_error& e) {
      throw DeserializeException("broken LuaJIT Script");
    }
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(4);

    pk.pack("ctx"s);
    pk.pack(path_ctx_);

    pk.pack("path"s);
    pk.pack(path_);

    pk.pack("shown"s);
    pk.pack(shown_);

    pk.pack("auto_compile"s);
    pk.pack(auto_compile_);
  }
  std::unique_ptr<File> Clone() const noexcept override {
    return std::make_unique<LuaJITScript>(path_ctx_, path_, shown_, auto_compile_);
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

 private:
  struct Data final {
    std::mutex  mtx;
    std::string msg = "not compiled yet";

    Time lastmod;

    int reg_func = LUA_REFNIL;
  };
  std::shared_ptr<Data> data_;

  // permanentized params
  std::string path_ctx_;
  std::string path_;

  bool shown_        = false;
  bool auto_compile_ = false;

  // volatile params
  std::string path_ctx_editing_;
  std::string path_editing_;


  void Compile(File::RefStack& ref) noexcept {
    // fetch script
    LuaJIT* ctx;
    std::shared_ptr<std::string> script;
    try {
      ctx = LuaJIT::Find(ref, path_ctx_);

      auto factory = File::iface<iface::Factory<Value>>(&*ref.Resolve(path_));
      if (!factory) throw Exception("no factory interface for Value");
      script = factory->Create().get<std::shared_ptr<Value::String>>();

    } catch (Exception& e) {
      std::unique_lock<std::mutex> k(data_->mtx);
      data_->msg = e.msg();
      return;
    }

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
    ctx->Queue(std::move(task));
  }
};
void LuaJITScript::Update(File::RefStack& ref) noexcept {
  const auto id = ref.Stringify() + ": LuaJIT Script Compiler";
  if (shown_) {
    if (ImGui::Begin(id.c_str())) {
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

  if (ImGui::BeginMenu("context path")) {
    gui::InputPathMenu(ref, &path_ctx_editing_, &path_ctx_);
    ImGui::EndMenu();
  }
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
  ImGui::Text("ctx      : %s", path_ctx_.c_str());
  ImGui::Text("path     : %s", path_.c_str());
  ImGui::Text("lastmod  : %s", std::ctime(&t));
  ImGui::Text("available: %s", data_->reg_func == LUA_REFNIL? "no": "yes");
  ImGui::Text("status   : %s", data_->msg.c_str());
}


class LuaJITNode : public File, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<LuaJITNode>(
      "LuaJITNode", "Node driven by LuaJIT",
      {typeid(iface::Node)});

  LuaJITNode() noexcept : File(&type_), Node(kNone) {
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

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node>(t).Select(this);
  }

 private:
  // permanentized params
  std::string path_;
};
void LuaJITNode::Update(File::RefStack&, const std::shared_ptr<Context>&) noexcept {
  ImGui::TextUnformatted("LuaJIT Node");
}

} }  // namespace kingtaker
