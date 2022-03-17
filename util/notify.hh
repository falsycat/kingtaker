#pragma once

#include "kingtaker.hh"


// the archtecture is inspired by ImGui
namespace kingtaker::notify {

enum Level {
  kTrace,
  kInfo,
  kWarn,
  kError,
};
static inline const char* StringifyLevel(Level lv) noexcept {
  return
      lv == kTrace? "TRAC":
      lv == kInfo?  "INFO":
      lv == kWarn?  "WARN":
      lv == kError? "ERRR": "?";
}

struct Item final {
 public:
  using RefStack = File::RefStack;

  Item() = default;
  Item(std::source_location src_,
       Level                lv_,
       std::string_view     text_,
       File::Path&&         path_ = {},
       File*                fptr_ = nullptr) noexcept :
    src(src_), lv(lv_), text(text_), path(std::move(path_)), fptr(fptr_),
    time(File::Clock::now()) {
  }
  Item(std::source_location src_,
       Level                lv_,
       std::string_view     text_,
       const RefStack&      ref_) noexcept :
    Item(src_, lv_, text_, ref_.GetFullPath(), &*ref_) {
  }
  Item(const Item&) = delete;
  Item(Item&&) = default;
  Item& operator=(const Item&) = delete;
  Item& operator=(Item&&) = default;

  // Clang doesn't have default constructor for source_location :(
  std::source_location src = std::source_location::current();

  Level lv = kInfo;
  std::string text;

  File::Path path;
  File* fptr = nullptr;  // don't refer the entity (use it as ID)

  File::Time time;

  bool select = false;
};


void Push(Item&&) noexcept;
void UpdateLogger(std::string_view filter = "", bool autoscroll = true) noexcept;


#define NOTIFY_SRCLOC_ std::source_location src = std::source_location::current()
static inline void Trace(
    const File::Path& p, File* fptr, std::string_view text, NOTIFY_SRCLOC_) noexcept {
  Push(Item {src, kTrace, text, File::Path(p), fptr});
}
static inline void Trace(
    const Item::RefStack& r, std::string_view text, NOTIFY_SRCLOC_) noexcept {
  Push(Item {src, kTrace, text, r});
}
static inline void Info(
    const File::Path& p, File* fptr, std::string_view text, NOTIFY_SRCLOC_) noexcept {
  Push(Item {src, kInfo, text, File::Path(p), fptr});
}
static inline void Info(
    const Item::RefStack& r, std::string_view text, NOTIFY_SRCLOC_) noexcept {
  Push(Item {src, kInfo, text, r});
}
static inline void Warn(
    const File::Path& p, File* fptr, std::string_view text, NOTIFY_SRCLOC_) noexcept {
  Push(Item {src, kWarn, text, File::Path(p), fptr});
}
static inline void Warn(
    const Item::RefStack& r, std::string_view text, NOTIFY_SRCLOC_) noexcept {
  Push(Item {src, kWarn, text, r});
}
static inline void Error(
    const File::Path& p, File* fptr, std::string_view text, NOTIFY_SRCLOC_) noexcept {
  Push(Item {src, kError, text, File::Path(p), fptr});
}
static inline void Error(
    const Item::RefStack& r, std::string_view text, NOTIFY_SRCLOC_) noexcept {
  Push(Item {src, kError, text, r});
}
#undef NOTIFY_SRCLOC_

}  // namespace kingtaker::notify
