#pragma once

#include "kingtaker.hh"


// the archtecture is inspired by ImGui
namespace kingtaker::notify {

enum Level {
  kInfo,
  kWarn,
  kError,
};
static inline const char* StringifyLevel(Level lv) noexcept {
  return
      lv == kInfo?  "INFO":
      lv == kWarn?  "WARN":
      lv == kError? "ERRR": "?";
}

struct Item final {
 public:
  using RefStack = File::RefStack;

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

  std::source_location src;

  Level lv;
  std::string text;

  File::Path path;
  File* fptr;  // don't refer the entity (use it as ID)

  File::Time time;

  bool select = false;
};


void Push(Item&&) noexcept;
void UpdateLogger(std::string_view filter = "", bool autoscroll = true) noexcept;


#define NOTIFY_SRCLOC_ std::source_location src = std::source_location::current()
static inline void Info(
    const Item::RefStack& r, std::string_view text, NOTIFY_SRCLOC_) noexcept {
  Push(Item {src, kInfo, text, r});
}
static inline void Warn(
    const Item::RefStack& r, std::string_view text, NOTIFY_SRCLOC_) noexcept {
  Push(Item {src, kWarn, text, r});
}
static inline void Error(
    const Item::RefStack& r, std::string_view text, NOTIFY_SRCLOC_) noexcept {
  Push(Item {src, kError, text, r});
}
#undef NOTIFY_SRCLOC_

}  // namespace kingtaker::notify
