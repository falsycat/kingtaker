#include "util/notify.hh"

#include <array>
#include <cinttypes>
#include <cmath>
#include <ctime>
#include <mutex>
#include <sstream>

#include <imgui.h>
#include <imgui_internal.h>

#include "util/format.hh"


namespace kingtaker::notify {

constexpr size_t N = 1000;

static std::mutex          mtx_;
static std::array<Item, N> logs_;
static size_t head_ = 0, tail_ = 0;


void Push(Item&& item) noexcept {
  std::unique_lock<std::mutex> k(mtx_);

  logs_[tail_%N] = std::move(item);
  tail_ = (tail_+1)%N;
  if (head_ == tail_) {
    head_ = (head_+1)%N;
  }
}

static void FocusAll(File::Event& ev) noexcept {
  for (size_t i = head_; i != tail_; i = (i+1)%N) {
    if (logs_[i].select) ev.Focus(logs_[i].fptr);
  }
  
}
static void CopyAll() noexcept {
  std::stringstream ret;
  for (size_t i = head_; i != tail_; i = (i+1)%N) {
    const auto& item = logs_[i];
    if (!item.select) continue;

    ret << StringifyTime(item.time)       << "|";
    ret << StringifyLevel(item.lv)        << "|";
    ret << item.text                      << "|";
    ret << File::StringifyPath(item.path) << "|";
    ret << item.src.file_name()           << ":";
    ret << item.src.line()                << ":";
    ret << item.src.column()              << "|";
    ret << item.src.function_name()       << "\n";
  }
  ImGui::SetClipboardText(ret.str().c_str());
}
static void Select(Item& item) noexcept {
  const auto& io = ImGui::GetIO();
  if (!(io.KeyMods & ImGuiKeyModFlags_Ctrl)) {
    for (auto& i : logs_) i.select = false;
  }
  item.select = true;
}
static bool Filter(const Item& item, std::string_view filter) noexcept {
  if (filter.empty()) return true;

  if (item.text.find(filter) != std::string::npos) return true;

  for (const auto& term : item.path) {
    if (term.find(filter) != std::string::npos) return true;
  }
  return false;
}

void UpdateLogger(
    File::Event& ev, std::string_view filter, bool autoscroll) noexcept {
  constexpr auto kTableFlags =
      ImGuiTableFlags_Resizable |
      ImGuiTableFlags_Hideable |
      ImGuiTableFlags_RowBg |
      ImGuiTableFlags_Borders |
      ImGuiTableFlags_ContextMenuInBody |
      ImGuiTableFlags_SizingStretchProp |
      ImGuiTableFlags_ScrollY;
  if (!ImGui::BeginTable("list", 6, kTableFlags, ImGui::GetContentRegionAvail(), 0)) {
    return;
  }
  ImGui::TableSetupColumn("time");
  ImGui::TableSetupColumn("level");
  ImGui::TableSetupColumn("text");
  ImGui::TableSetupColumn("file");
  ImGui::TableSetupColumn("location");
  ImGui::TableSetupColumn("function");
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableHeadersRow();

  const auto now = Clock::now();

  const auto bg      = ImGui::GetStyleColorVec4(ImGuiCol_TableRowBg);
  const auto fg_high = ImVec4(1, 1, 1, 1);

  std::unique_lock<std::mutex> k(mtx_);
  for (size_t i = head_; i != tail_; i = (i+1)%N) {
    auto& item = logs_[i];
    if (!Filter(item, filter)) continue;

    ImGui::PushID(&item);
    ImGui::TableNextRow();

    const auto bg_high =
        item.lv == kWarn?  ImVec4(.94f, .65f, .00f, 1):
        item.lv == kError? ImVec4(.89f, .35f, .15f, 1): bg;
    const auto fg =
        item.lv == kWarn?  ImVec4(.94f, .65f, .00f, 1):
        item.lv == kError? ImVec4(.89f, .35f, .15f, 1): fg_high;

    const auto elap    = std::chrono::duration_cast<std::chrono::milliseconds>(now-item.time).count();
    const auto appear  = std::min(1.f, static_cast<float>(elap)/5000.f);
    const auto appear4 = ImVec4(appear, appear, appear, appear);
    const auto bg_calc = ImGui::ColorConvertFloat4ToU32((bg-bg_high)*appear4+bg_high);
    const auto fg_calc = ImGui::ColorConvertFloat4ToU32((fg-fg_high)*appear4+fg_high);
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, bg_calc);

    if (ImGui::TableSetColumnIndex(0)) {
      constexpr auto kFlags =
          ImGuiSelectableFlags_SpanAllColumns |
          ImGuiSelectableFlags_AllowItemOverlap;
      if (ImGui::Selectable(StringifyTime(item.time).c_str(), item.select, kFlags)) {
        Select(item);
      }
      if (autoscroll && (i+1)%N == tail_) {
        ImGui::SetScrollHereY();
      }
      if (ImGui::BeginPopupContextItem()) {
        Select(item);
        if (ImGui::MenuItem("focus")) {
          FocusAll(ev);
        }
        if (ImGui::MenuItem("copy as text")) {
          CopyAll();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("deselect all")) {
          for (auto& it : logs_) it.select = false;
        }
        if (ImGui::MenuItem("select all")) {
          for (auto& it : logs_) it.select = true;
        }
        ImGui::EndPopup();
      }
    }
    if (ImGui::TableNextColumn()) {
      ImGui::PushStyleColor(ImGuiCol_Text, fg_calc);
      ImGui::TextUnformatted(StringifyLevel(item.lv));
      ImGui::PopStyleColor(1);
    }
    if (ImGui::TableNextColumn()) {
      ImGui::TextUnformatted(item.text.c_str());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", item.text.c_str());
      }
    }
    if (ImGui::TableNextColumn()) {
      const auto path = File::StringifyPath(item.path);
      ImGui::TextUnformatted(path.c_str());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", path.c_str());
      }
    }
    if (ImGui::TableNextColumn()) {
      ImGui::Text("%s:%" PRIuMAX ":%" PRIuMAX,
                  item.src.file_name(),
                  static_cast<uintmax_t>(item.src.line()),
                  static_cast<uintmax_t>(item.src.column()));
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s:%" PRIuMAX ":%" PRIuMAX,
                          item.src.file_name(),
                          static_cast<uintmax_t>(item.src.line()),
                          static_cast<uintmax_t>(item.src.column()));
      }
    }
    if (ImGui::TableNextColumn()) {
      ImGui::Text("%s", item.src.function_name());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", item.src.function_name());
      }
    }
    ImGui::PopID();
  }
  ImGui::EndTable();
}


}  // namespace kingtaker::notify
