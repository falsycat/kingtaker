#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <source_location.hh>

#include "iface/logger.hh"
#include "iface/node.hh"


namespace kingtaker {

class NodeLoggerItem : public iface::Logger::Item {
 public:
  NodeLoggerItem(iface::Logger::Level      lv,
                 File::Path&&              path,
                 std::vector<File::Path>&& strace,
                 std::source_location      srcloc) noexcept :
      Item(lv, std::move(path), srcloc), strace_(std::move(strace)) {
  }

  void UpdateTooltip(File&) noexcept override;
  void UpdateMenu(File&) noexcept override;

  std::string Stringify() const noexcept override;

 private:
  std::vector<File::Path> strace_;
};


class NodeLoggerTextItem : public NodeLoggerItem {
 public:
# define KINGTAKER_UTIL_NODE_LOGGER_DEFINE_LEVEL_(N)  \
      static void N(File::Path&&          path,  \
                    iface::Node::Context& ctx,  \
                    std::string_view      msg,  \
                    std::source_location  srcloc = std::source_location::current()) noexcept {  \
        ctx.Notify(std::make_shared<NodeLoggerTextItem>(  \
                iface::Logger::k##N, std::move(path), ctx.GetStackTrace(), msg, srcloc));  \
      }  \
      static void N(const File::Path&     path,  \
                    iface::Node::Context& ctx,  \
                    std::string_view      msg,  \
                    std::source_location  srcloc = std::source_location::current()) noexcept {  \
        N(File::Path(path), ctx, msg, srcloc);  \
      }
  KINGTAKER_UTIL_NODE_LOGGER_DEFINE_LEVEL_(Info)
  KINGTAKER_UTIL_NODE_LOGGER_DEFINE_LEVEL_(Warn)
  KINGTAKER_UTIL_NODE_LOGGER_DEFINE_LEVEL_(Error)
# undef KINGTAKER_UTIL_NODE_LOGGER_DEFINE_LEVEL_

  NodeLoggerTextItem(iface::Logger::Level      lv,
                     File::Path&&              path,
                     std::vector<File::Path>&& strace,
                     std::string_view          msg,
                     std::source_location      srcloc) noexcept :
      NodeLoggerItem(lv, std::move(path), std::move(strace), srcloc), msg_(msg) {
  }

  void UpdateSummary(File&) noexcept override;
  void UpdateTooltip(File&) noexcept override;

  std::string Stringify() const noexcept override;

 private:
  std::string msg_;
};

}  // namespace kingtaker
