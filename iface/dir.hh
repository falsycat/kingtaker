#pragma once

#include "kingtaker.hh"

namespace kingtaker::iface {

class Dir {
 public:
  static Dir& null() noexcept { static Dir instance; return instance; }

  static bool ValidateName(std::string_view name) {
    static const std::string kAllowed =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_.0123456789";
    for (auto c : name) {
      if (kAllowed.find(c) == std::string::npos) return false;
    }
    return true;
  }

  Dir() = default;
  virtual ~Dir() = default;
  Dir(const Dir&) = delete;
  Dir(Dir&&) = delete;
  Dir& operator=(const Dir&) = delete;
  Dir& operator=(Dir&&) = delete;

  virtual File* Add(std::string_view, std::unique_ptr<File>&&) noexcept { return nullptr; }
  virtual std::unique_ptr<File> Remove(std::string_view) noexcept { return nullptr; }

  virtual size_t size() const noexcept { return 0; }
};

class DirItem {
 public:
  static DirItem& null() noexcept { static DirItem inst_(0); return inst_; }

  enum Flag : uint8_t {
    kNone    = 0,
    kTree    = 0b001,
    kMenu    = 0b010,
    kTooltip = 0b100,
  };
  using Flags = uint8_t;

  DirItem() = delete;
  DirItem(Flags f) : flags_(f) { }
  virtual ~DirItem() = default;
  DirItem(const DirItem&) = delete;
  DirItem(DirItem&&) = delete;
  DirItem& operator=(const DirItem&) = delete;
  DirItem& operator=(DirItem&&) = delete;

  virtual void UpdateTree() noexcept { }
  virtual void UpdateMenu() noexcept { }
  virtual void UpdateTooltip() noexcept { }

  Flags flags() const noexcept { return flags_; }
 private:
  Flags flags_;
};

}  // namespace kingtaker::iface
