#pragma once

#include <algorithm>
#include <string>

#include <imgui.h>

#include "kingtaker.hh"


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


bool BeginWindow(
    File*,
    const char*,
    const File::Event&,
    bool*,
    ImGuiWindowFlags = 0) noexcept;
void EndWindow() noexcept;


void NodeSockPoint() noexcept;
void NodeInSock(const std::string&, const std::string& = "") noexcept;
void NodeOutSock(const std::string&, const std::string& = "") noexcept;

void NodeCanvasSetZoom() noexcept;
void NodeCanvasResetZoom() noexcept;


File* InputPathMenu(const char* id, File*, std::string* str) noexcept;


inline float CalcTextMaxWidth(const auto& list, const auto& func) noexcept {
  float ret = 0;
  for (const auto& item : list) {
    ret = std::max(ret, ImGui::CalcTextSize(func(item)).x);
  }
  return ret;
}

}  // namespace kingtaker::gui
