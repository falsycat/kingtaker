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
  static std::vector<File::Path> GetStackTrace(
      File::Path&& path, const iface::Node::Context& ctx) noexcept {
    auto ret = ctx.GetStackTrace();
    ret.insert(ret.begin(), std::move(path));
    return ret;
  }

  NodeLoggerItem(iface::Logger::Level      lv,
                 std::vector<File::Path>&& strace,
                 std::source_location srcloc = std::source_location::current()) noexcept :
      Item(lv, srcloc), strace_(std::move(strace)) {
  }

  void UpdateTooltip() noexcept override;
  void UpdateMenu() noexcept override;

  std::string Stringify() const noexcept override;

 private:
  std::vector<File::Path> strace_;
};


class NodeLoggerTextItem : public NodeLoggerItem {
 public:
  static void Info(File::Path&&          path,
                   iface::Node::Context& ctx,
                   std::string_view      msg,
                   std::source_location  srcloc = std::source_location::current()) noexcept {
    ctx.Notify(std::make_shared<NodeLoggerTextItem>(
            iface::Logger::kInfo, GetStackTrace(std::move(path), ctx), msg, srcloc));
  }
  static void Info(const File::Path&     path,
                   iface::Node::Context& ctx,
                   std::string_view      msg,
                   std::source_location  srcloc = std::source_location::current()) noexcept {
    Info(File::Path(path), ctx, msg, srcloc);
  }
  static void Warn(File::Path&&          path,
                   iface::Node::Context& ctx,
                   std::string_view      msg,
                   std::source_location  srcloc = std::source_location::current()) noexcept {
    ctx.Notify(std::make_shared<NodeLoggerTextItem>(
            iface::Logger::kWarn, GetStackTrace(std::move(path), ctx), msg, srcloc));
  }
  static void Warn(const File::Path&     path,
                   iface::Node::Context& ctx,
                   std::string_view      msg,
                   std::source_location  srcloc = std::source_location::current()) noexcept {
    Warn(File::Path(path), ctx, msg, srcloc);
  }
  static void Error(File::Path&&          path,
                    iface::Node::Context& ctx,
                    std::string_view      msg,
                    std::source_location  srcloc = std::source_location::current()) noexcept {
    ctx.Notify(std::make_shared<NodeLoggerTextItem>(
            iface::Logger::kError, GetStackTrace(std::move(path), ctx), msg, srcloc));
  }
  static void Error(const File::Path&     path,
                    iface::Node::Context& ctx,
                    std::string_view      msg,
                    std::source_location  srcloc = std::source_location::current()) noexcept {
    Error(File::Path(path), ctx, msg, srcloc);
  }

  NodeLoggerTextItem(iface::Logger::Level      lv,
                     std::vector<File::Path>&& strace,
                     std::string_view          msg,
                     std::source_location srcloc = std::source_location::current()) noexcept :
      NodeLoggerItem(lv, std::move(strace), srcloc), msg_(msg) {
  }

  void UpdateSummary() noexcept override;
  std::string Stringify() const noexcept override;

 private:
  std::string msg_;
};

}  // namespace kingtaker
