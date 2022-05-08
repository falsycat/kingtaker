#pragma once

#include <memory>

#include <source_location.hh>

#include "kingtaker.hh"


namespace kingtaker::iface {

class Logger {
 public:
  class Item;

  enum Level {
    kInfo,
    kWarn,
    kError,
  };

  Logger() = default;
  virtual ~Logger() = default;
  Logger(const Logger&) = delete;
  Logger(Logger&&) = delete;
  Logger& operator=(const Logger&) = delete;
  Logger& operator=(Logger&&) = delete;

  virtual void Push(const std::shared_ptr<Item>&) noexcept = 0;
};

// items should be independent from any files
class Logger::Item {
 public:
  Item() = delete;
  Item(Level lv, File::Path&& path, std::source_location loc) noexcept :
      lv_(lv), path_(std::move(path)), srcloc_(loc) {
  }
  virtual ~Item() = default;
  Item(const Item&) = delete;
  Item(Item&&) = delete;
  Item& operator=(const Item&) = delete;
  Item& operator=(Item&&) = delete;

  virtual void UpdateSummary(File&) noexcept { }
  virtual void UpdateTooltip(File&) noexcept { }
  virtual void UpdateMenu(File&) noexcept { }

  virtual std::string Stringify() const noexcept = 0;

  Level lv() const noexcept { return lv_; }
  const File::Path& path() const noexcept { return path_; }
  const std::source_location& srcloc() const noexcept { return srcloc_; }

 private:
  Level lv_;
  File::Path path_;
  std::source_location srcloc_;
};

}  // namespace kingtaker::iface
