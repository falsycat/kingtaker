#pragma once

#include <imgui.h>


namespace kingtaker::gui {

class ResizeGroup final {
 public:
  ResizeGroup() = delete;
  ResizeGroup(const char* id, ImVec2* size, const ImVec2& min, const ImVec2& max, float scale) noexcept;
  ~ResizeGroup() noexcept;
  ResizeGroup(const ResizeGroup&) = delete;
  ResizeGroup(ResizeGroup&&) = delete;
  ResizeGroup& operator=(const ResizeGroup&) = delete;
  ResizeGroup& operator=(ResizeGroup&&) = delete;

 private:
  ImVec2  size_;
  ImVec2* out_;
};


void NodeSocket() noexcept;
void NodeCanvasSetZoom() noexcept;
void NodeCanvasResetZoom() noexcept;

}  // namespace kingtaker::gui
