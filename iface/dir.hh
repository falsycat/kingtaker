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

}  // namespace kingtaker::iface
