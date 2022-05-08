#include "util/node_logger.hh"

#include <sstream>


namespace kingtaker {

void NodeLoggerItem::UpdateTooltip() noexcept {
  ImGui::TextUnformatted("====STACKTRACE====");
  ImGui::Indent();
  const size_t depth = strace_.size();
  for (size_t i = 0; i < 6 && i < depth; ++i) {
    ImGui::Text("%zu. %s",
                static_cast<size_t>(depth-i-1),
                strace_[i].Stringify().c_str());
  }
  ImGui::Unindent();
}
void NodeLoggerItem::UpdateMenu() noexcept {
  const size_t depth = strace_.size();
  if (depth < 16) {
    for (size_t i = 0; i < depth; ++i) {
      const auto str = std::to_string(depth-i-1) + strace_[i].Stringify();
      if (ImGui::MenuItem(str.c_str())) {
        // TODO focus
      }
    }
  } else {
    ImGui::MenuItem("Not Implemented X(");
    // TODO support dumping of deep stack
  }
}
std::string NodeLoggerItem::Stringify() const noexcept {
  std::stringstream st;
  st << "====STACKTRACE====" << "\n";
  for (const auto& path : strace_) {
    st << "  " << path.Stringify() << "\n";
  }
  return st.str();
}


void NodeLoggerTextItem::UpdateSummary() noexcept {
  ImGui::TextUnformatted(msg_.c_str());
}
std::string NodeLoggerTextItem::Stringify() const noexcept {
  std::stringstream st;
  st << msg_ << "\n";
  st << NodeLoggerItem::Stringify();
  return st.str();
}

}  // namespace kingtaker
