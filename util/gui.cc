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


void NodeSocket() noexcept {
  const auto em     = ImGui::GetFontSize();
  const auto radius = em/2 / ImNodes::CanvasState().Zoom;
  const auto radvec = ImVec2(radius, radius);
  const auto pos    = ImGui::GetCursorScreenPos();

  auto dlist = ImGui::GetWindowDrawList();
  dlist->AddCircleFilled(
      pos+radvec, radius, IM_COL32(100, 100, 100, 100));
  dlist->AddCircleFilled(
      pos+radvec, radius*.8f, IM_COL32(200, 200, 200, 200));

  ImGui::Dummy(radvec*2);
}

void NodeCanvasSetZoom() noexcept {
  auto ctx = ImNodes::GetCurrentCanvas();
  assert(ctx);

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
      *path = std::move(*editing);
      return true;
    }
  } catch (File::NotFoundException&) {
    ImGui::Bullet();
    ImGui::TextUnformatted("file not found");
  }
  return false;
}

}  // namespace kingtaker::gui
