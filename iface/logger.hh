#pragma once

#include <memory>

#include <source_location.hh>


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

  virtual void Push(std::shared_ptr<Item>&&) noexcept = 0;
};

// items should be independent from any files
class Logger::Item {
 public:
  Item() = delete;
  Item(Level lv, std::source_location loc) noexcept : lv_(lv), srcloc_(loc) {
  }
  virtual ~Item() = default;
  Item(const Item&) = delete;
  Item(Item&&) = delete;
  Item& operator=(const Item&) = delete;
  Item& operator=(Item&&) = delete;

  virtual void UpdateSummary() noexcept { }
  virtual void UpdateTooltip() noexcept { }
  virtual void UpdateMenu() noexcept { }

  virtual std::string Stringify() const noexcept = 0;

  Level lv() const noexcept { return lv_; }
  const std::source_location& srcloc() const noexcept { return srcloc_; }

 private:
  Level lv_;
  std::source_location srcloc_;
};

}  // namespace kingtaker::iface
