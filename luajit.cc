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
#include "iface/gui.hh"
#include "iface/node.hh"

#include "util/ptr_selector.hh"


namespace kingtaker {
namespace {

// LuaJIT context file which owns lua_State*
class LuaJIT : public File, public iface::GUI, public iface::DirItem {
 public:
  static inline TypeInfo type_ = TypeInfo::New<LuaJIT>(
      "LuaJIT", "LuaJIT module",
      {typeid(iface::DirItem), typeid(iface::GUI)});


  using Command = std::function<void()>;


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
  std::vector<std::string> logs_;
  std::string inline_expr_;
  std::string inline_result_;


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
          cmd();
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

    auto str = luaL_checkstring(L, 1);
    f->logs_.push_back(str);
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
      Queue([this, expr = inline_expr_]() noexcept {
        luaL_dostring(L, ("return "s + expr).c_str());

        std::string ret;
        if (lua_isstring(L, -1)) {
          ret = lua_tostring(L, -1);
        } else {
          ret = lua_typename(L, lua_type(L, -1));
        }
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

} }  // namespace kingtaker
