#include "util/notify.hh"

#include <cinttypes>
#include <cmath>
#include <ctime>
#include <mutex>

#include <imgui.h>
#include <imgui_internal.h>

#include "iface/gui.hh"


namespace kingtaker::notify {

static std::mutex       mtx_;
static std::deque<Item> logs_;


void Push(Item&& item) noexcept {
  std::unique_lock<std::mutex> k(mtx_);
  logs_.push_back(std::move(item));
}

static void FocusAll() noexcept {
  std::unordered_set<std::string> paths;
  for (const auto& item : logs_) {
    if (item.select) {
      paths.insert(File::StringifyPath(item.path));
    }
  }
  for (const auto& p : paths) {
    try {
      auto target = File::RefStack().Resolve(p);
      auto ref    = target;
      for (;;) {
        auto g = File::iface<iface::GUI>(&*ref);
        if (g && g->OnFocus(target, ref.size())) break;
        if (ref.size() == 0) break;
        ref.Pop();
      }
    } catch (File::NotFoundException&) {
    }
  }
}
static void CopyAll() noexcept {
  // TODO
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

void UpdateLogger(std::string_view filter, bool autoscroll) noexcept {
  constexpr auto kTableFlags =
      ImGuiTableFlags_Resizable |
      ImGuiTableFlags_Hideable |
      ImGuiTableFlags_RowBg |
      ImGuiTableFlags_Borders |
      ImGuiTableFlags_ContextMenuInBody |
      ImGuiTableFlags_SizingStretchProp |
      ImGuiTableFlags_ScrollY;
  if (!ImGui::BeginTable("list", 7, kTableFlags, ImGui::GetContentRegionAvail(), 0)) {
    return;
  }
  ImGui::TableSetupColumn("time");
  ImGui::TableSetupColumn("level");
  ImGui::TableSetupColumn("text");
  ImGui::TableSetupColumn("file");
  ImGui::TableSetupColumn("src");
  ImGui::TableSetupColumn("location");
  ImGui::TableSetupColumn("function");
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableHeadersRow();

  const auto now = File::Clock::now();

  const auto bg_high = ImVec4(1, 0, 0, 1);
  const auto bg      = ImGui::GetStyleColorVec4(ImGuiCol_TableRowBg);

  std::unique_lock<std::mutex> k(mtx_);
  for (auto& item : logs_) {
    if (!Filter(item, filter)) continue;

    ImGui::PushID(&item);
    ImGui::TableNextRow();

    const auto elap   = std::chrono::duration_cast<std::chrono::milliseconds>(now-item.time).count();
    const auto appear = std::min(1.f, static_cast<float>(elap)/5000.f);
    const auto ap4    = ImVec4(appear, appear, appear, appear);
    const auto col    = ImGui::ColorConvertFloat4ToU32(ap4*(bg-bg_high)+bg_high);
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, col);

    if (ImGui::TableSetColumnIndex(0)) {
      const auto dur = item.time.time_since_epoch();

      char buf[64];
      snprintf(
          buf, sizeof(buf),
          "%02" PRIuMAX ":%02" PRIuMAX ":%02" PRIuMAX ".%03" PRIuMAX,
          static_cast<uintmax_t>(std::chrono::duration_cast<std::chrono::hours>(dur).count())%24,
          static_cast<uintmax_t>(std::chrono::duration_cast<std::chrono::minutes>(dur).count())%60,
          static_cast<uintmax_t>(std::chrono::duration_cast<std::chrono::seconds>(dur).count())%60,
          static_cast<uintmax_t>(std::chrono::duration_cast<std::chrono::milliseconds>(dur).count())%1000);

      constexpr auto kFlags =
          ImGuiSelectableFlags_SpanAllColumns |
          ImGuiSelectableFlags_AllowItemOverlap;
      if (ImGui::Selectable(buf, item.select, kFlags)) {
        Select(item);
      }
      if (autoscroll && &item == &logs_.back()) {
        ImGui::SetScrollHereY();
      }
      if (ImGui::BeginPopupContextItem()) {
        Select(item);
        if (ImGui::MenuItem("focus")) {
          FocusAll();
        }
        if (ImGui::MenuItem("copy as text")) {
          CopyAll();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("deselect all")) {
          for (auto& i : logs_) i.select = false;
        }
        if (ImGui::MenuItem("select all")) {
          for (auto& i : logs_) i.select = true;
        }
        ImGui::EndPopup();
      }
    }
    if (ImGui::TableNextColumn()) {
      const char* lvstr =
          item.lv == kInfo? "INFO":
          item.lv == kWarn? "WARN":
          item.lv == kWarn? "ERRR": "?";
      ImGui::TextUnformatted(lvstr);
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
      ImGui::TextUnformatted(item.src.file_name());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", item.src.file_name());
      }
    }
    if (ImGui::TableNextColumn()) {
      ImGui::Text("%" PRIuMAX ":%" PRIuMAX,
                  static_cast<uintmax_t>(item.src.line()),
                  static_cast<uintmax_t>(item.src.column()));
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
