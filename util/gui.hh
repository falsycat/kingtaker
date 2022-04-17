#pragma once

#include <string>

#include <imgui.h>

#include "kingtaker.hh"

#include "iface/node.hh"


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
    const File::RefStack&,
    const File::Event&,
    bool*,
    ImGuiWindowFlags = 0) noexcept;
void EndWindow() noexcept;


void NodeSocket() noexcept;
void NodeInSock(const std::string&) noexcept;
void NodeInSock(
    const std::shared_ptr<iface::Node::Context>&,
    const std::shared_ptr<iface::Node::InSock>&) noexcept;
void NodeOutSock(const std::string&) noexcept;

void NodeCanvasSetZoom() noexcept;
void NodeCanvasResetZoom() noexcept;


bool InputPathMenu(File::RefStack&, std::string* editing, std::string* path) noexcept;

void TextCenterChopped(std::string_view, float w) noexcept;

}  // namespace kingtaker::gui
