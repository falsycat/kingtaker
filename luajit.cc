#include "kingtaker.hh"

#include <lua.hpp>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "iface/dir.hh"
#include "iface/gui.hh"
#include "iface/node.hh"


namespace kingtaker {
namespace {

class LuaJIT : public File, public iface::GUI, public iface::DirItem {
 public:
  static inline TypeInfo type_ = TypeInfo::New<LuaJIT>(
      "LuaJIT", "LuaJIT module",
      {typeid(iface::DirItem), typeid(iface::GUI)});

  LuaJIT() noexcept : File(&type_), DirItem(kMenu) {
    L = luaL_newstate();
    SetupBuiltinFeatures();
  }
  ~LuaJIT() noexcept {
    if (L) lua_close(L);
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

  void Update(RefStack& ref) noexcept override {
    const auto id = ref.Stringify() + ": LuaJIT Context Inspector";
    if (shown_) {
      if (ImGui::Begin(id.c_str())) {
        UpdateInspector(ref);
      }
      ImGui::End();
    }
  }
  void UpdateMenu(RefStack&) noexcept override {
    ImGui::MenuItem("Context Inspector", nullptr, &shown_);
  }
  void UpdateInspector(RefStack&) noexcept {
    const auto& style = ImGui::GetStyle();

    constexpr auto kHeaderFlags = ImGuiTreeNodeFlags_DefaultOpen;
    if (!L) {
      ImGui::Text("Context creation failed!!");
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
        luaL_dostring(L, ("return "s + inline_expr_).c_str());
        if (lua_isstring(L, -1)) {
          inline_result_ = lua_tostring(L, -1);
        } else {
          inline_result_ = lua_typename(L, lua_type(L, -1));
        }
        inline_result_ = "-> "s + inline_result_;
      }
      if (inline_result_.size()) {
        ImGui::TextWrapped(inline_result_.c_str());
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

  void* iface(const std::type_index& t) noexcept {
    if (t == typeid(iface::GUI))     return static_cast<iface::GUI*>(this);
    if (t == typeid(iface::DirItem)) return static_cast<iface::DirItem*>(this);
    return nullptr;
  }

 private:
  lua_State* L = nullptr;

  // permanentized parameters
  bool shown_ = false;

  // volatile parameters
  std::vector<std::string> logs_;
  std::string inline_expr_;
  std::string inline_result_;


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

} }  // namespace kingtaker
