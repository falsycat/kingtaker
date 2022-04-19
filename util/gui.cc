#include "util/gui.hh"

#include <algorithm>
#include <cassert>

#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <ImNodes.h>


namespace kingtaker::gui {

ResizeGroup::ResizeGroup(const char* id, ImVec2* size, const ImVec2& min, const ImVec2& max, float scale) noexcept :
    out_(size) {
  out_->x = std::clamp(out_->x, min.x, max.x);
  out_->y = std::clamp(out_->y, min.y, max.y);
  size_   = *out_;

  auto  ctx = ImGui::GetCurrentContext();
  auto& io  = ImGui::GetIO();

  ImGui::BeginGroup();

  const auto base = ImGui::GetCursorScreenPos();
  const auto pos  = base + *size*scale;
  const auto rc   = ImRect {pos.x-1*scale, pos.y-1*scale, pos.x, pos.y};

  bool hovered, held;
  ImGui::ButtonBehavior(rc, ImGui::GetID(id), &hovered, &held, ImGuiButtonFlags_FlattenChildren);
  if (hovered || held) ctx->MouseCursor = ImGuiMouseCursor_ResizeNESW;

  ImGuiCol col = ImGuiCol_Button;
  if (hovered) col = ImGuiCol_ButtonHovered;
  if (held) {
    col = ImGuiCol_ButtonActive;
    size_ = (io.MousePos + (ImVec2{scale, scale}-ctx->ActiveIdClickOffset) - base) / scale;
    size_.x = std::clamp(size_.x, min.x, max.x);
    size_.y = std::clamp(size_.y, min.y, max.y);
  }

  auto dlist = ImGui::GetWindowDrawList();
  dlist->AddTriangleFilled(pos, pos+ImVec2{0, -scale}, pos+ImVec2{-scale, 0}, ImGui::GetColorU32(col));
}
ResizeGroup::~ResizeGroup() noexcept {
  *out_ = size_;
  ImGui::EndGroup();
}


static bool BeginWindow_end_required_ = false;

bool BeginWindow(
    File*                 fptr,
    const char*           name,
    const File::RefStack& ref,
    const File::Event&    ev,
    bool*                 shown,
    ImGuiWindowFlags      flags) noexcept {
  BeginWindow_end_required_ = false;

  const auto id = ref.Stringify()+": "s+name;
  if (ev.IsFocused(fptr)) {
    ImGui::SetWindowFocus(id.c_str());
    *shown = true;
  }
  if (!*shown) {
    return false;
  }

  BeginWindow_end_required_ = true;
  return ImGui::Begin(id.c_str(), shown, flags);
}
void EndWindow() noexcept {
  if (BeginWindow_end_required_) ImGui::End();
}


void NodeSocket() noexcept {
  auto win = ImGui::GetCurrentWindow();

  const auto em  = ImGui::GetFontSize();
  const auto lh  = std::max(win->DC.CurrLineSize.y, em);
  const auto rad = em/2 / ImNodes::CanvasState().Zoom;
  const auto sz  = ImVec2 {rad*2, lh};
  const auto pos = ImGui::GetCursorScreenPos() + sz/2;

  auto dlist = ImGui::GetWindowDrawList();
  dlist->AddCircleFilled(
      pos, rad, IM_COL32(100, 100, 100, 100));
  dlist->AddCircleFilled(
      pos, rad*.8f, IM_COL32(200, 200, 200, 200));

  ImGui::Dummy(sz);
}
void NodeInSock(const std::string& name) noexcept {
  gui::NodeSocket();
  ImGui::SameLine();
  ImGui::TextUnformatted(name.c_str());
}
void NodeInSock(
    const std::shared_ptr<iface::Node::Context>& ctx,
    const std::shared_ptr<iface::Node::InSock>&  sock) noexcept {
  gui::NodeSocket();
  ImGui::SameLine();

  if (ImGui::SmallButton(sock->name().c_str())) {
    Queue::sub().Push([sock, ctx]() { sock->Receive(ctx, {}); });
  }
}
void NodeOutSock(const std::string& name) noexcept {
  ImGui::TextUnformatted(name.c_str());
  ImGui::SameLine();
  gui::NodeSocket();
}

void NodeCanvasSetZoom() noexcept {
  auto ctx = ImNodes::GetCurrentCanvas();
  if (!ctx) return;

  const auto z = ctx->Zoom;
  ImGui::SetWindowFontScale(z);

  auto& s = ImGui::GetStyle();
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,     s.FramePadding*z);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,    s.FrameRounding*z);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,  s.FrameBorderSize*z);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      s.ItemSpacing*z);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, s.ItemInnerSpacing*z);
  ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing,    s.IndentSpacing*z);
}
void NodeCanvasResetZoom() noexcept {
  auto ctx = ImNodes::GetCurrentCanvas();
  if (!ctx) return;

  ImGui::SetWindowFontScale(1.f);
  ImGui::PopStyleVar(6);
}


bool InputPathMenu(File::RefStack& ref, std::string* editing, std::string* path) noexcept {
  constexpr auto kFlags = 
      ImGuiInputTextFlags_EnterReturnsTrue |
      ImGuiInputTextFlags_AutoSelectAll;
  static const char* const kHint = "enter new path...";

  ImGui::SetKeyboardFocusHere();
  const bool submit = ImGui::InputTextWithHint("##InputPathMenu", kHint, editing, kFlags);
  if (ImGui::IsItemActivated()) *editing = *path;

  try {
    auto newref = ref.Resolve(*editing);
    if (submit) {
      ImGui::CloseCurrentPopup();
      if (*path == *editing) return false;

      *path = std::move(*editing);
      return true;
    }
  } catch (File::NotFoundException&) {
    ImGui::Bullet();
    ImGui::TextUnformatted("file not found");
  }
  return false;
}


void TextCenterChopped(std::string_view v, float w) noexcept {
  auto eol  = v.find('\n');
  auto line = v.substr(0, eol);

  auto trimmed = eol != std::string::npos;

  auto msg_w  = 0.f;
  auto dots_w = 0.f;
  for (;;) {
    msg_w = ImGui::CalcTextSize(&line.front(), &line.back()+1).x + dots_w;
    if (msg_w < w) break;
    if (dots_w == 0) dots_w = ImGui::CalcTextSize("...").x;
    line.remove_suffix(1);
  }
  ImGui::BeginGroup();
  ImGui::SetCursorPosX(ImGui::GetCursorPosX()+(w-msg_w)/2);
  if (trimmed) {
    ImGui::Text("%.*s...", static_cast<int>(line.size()), line.data());
  } else {
    ImGui::TextUnformatted(&line.front(), &line.back()+1);
  }
  ImGui::EndGroup();
  if (trimmed && ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%.*s", static_cast<int>(v.size()), v.data());
  }
}

}  // namespace kingtaker::gui
