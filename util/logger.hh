#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <source_location.hh>

#include "kingtaker.hh"

#include "iface/logger.hh"


namespace kingtaker {

class LoggerTemporaryItemQueue final {
 public:
  using Item = iface::Logger::Item;

  LoggerTemporaryItemQueue() = default;
  LoggerTemporaryItemQueue(const LoggerTemporaryItemQueue&) = default;
  LoggerTemporaryItemQueue(LoggerTemporaryItemQueue&&) = default;
  LoggerTemporaryItemQueue& operator=(const LoggerTemporaryItemQueue&) = default;
  LoggerTemporaryItemQueue& operator=(LoggerTemporaryItemQueue&&) = default;

  // thread-safe
  void Push(const std::shared_ptr<Item>& item) noexcept {
    std::unique_lock<std::mutex> _(mtx_);
    items_.push_back(item);
  }

  void Flush(File& base) noexcept {
    std::unique_lock<std::mutex> _(mtx_);
    if (items_.empty()) return;
    try {
      auto& f      = base.ResolveUpward("_logger");
      auto* logger = File::iface<iface::Logger>(&f);
      for (auto& item : items_) {
        logger->Push(std::move(item));
      }
      items_.clear();
    } catch (File::NotFoundException&) {
    }
  }

 private:
  std::mutex mtx_;

  std::vector<std::shared_ptr<Item>> items_;
};


class LoggerTextItem : public iface::Logger::Item {
 public:
# define KINGTAKER_UTIL_LOGGER_DEFINE_LEVEL_(N)  \
      static std::shared_ptr<LoggerTextItem> N(File::Path&&         path,  \
                                               std::string_view     msg,  \
                                               std::source_location srcloc = std::source_location::current()) noexcept {  \
        return std::make_shared<LoggerTextItem>(  \
            iface::Logger::k##N, std::move(path), msg, srcloc);  \
      }  \
      static std::shared_ptr<LoggerTextItem> N(const File::Path&    path,  \
                                               std::string_view     msg,  \
                                               std::source_location srcloc = std::source_location::current()) noexcept {  \
        return std::make_shared<LoggerTextItem>(  \
            iface::Logger::k##N, File::Path(path), msg, srcloc);  \
      }
  KINGTAKER_UTIL_LOGGER_DEFINE_LEVEL_(Info)
  KINGTAKER_UTIL_LOGGER_DEFINE_LEVEL_(Warn)
  KINGTAKER_UTIL_LOGGER_DEFINE_LEVEL_(Error)
# undef KINGTAKER_UTIL_LOGGER_DEFINE_LEVEL_

  LoggerTextItem() = delete;
  LoggerTextItem(iface::Logger::Level lv,
                 File::Path&&         path,
                 std::string_view     msg,
                 std::source_location srcloc) noexcept :
      Item(lv, srcloc), path_(std::move(path)), msg_(msg) {
  }

  void UpdateSummary() noexcept {
    ImGui::TextUnformatted(msg_.c_str());
  }
  void UpdateTooltip() noexcept {
    ImGui::TextUnformatted(msg_.c_str());
    ImGui::TextDisabled("from %s", path_.Stringify().c_str());
  }
  void UpdateMenu() noexcept {
    if (ImGui::MenuItem("focus")) {
      // TODO
    }
  }

  std::string Stringify() const noexcept {
    return msg_;
  }

 private:
  File::Path path_;
  std::string msg_;
};

}  // namespace kingtaker
