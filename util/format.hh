#pragma once

#include <chrono>
#include <cstring>
#include <cinttypes>

#include "kingtaker.hh"


namespace kingtaker {

std::string StringifyTime(File::Time t) noexcept {
  const auto dur = t.time_since_epoch();

  char buf[64];
  snprintf(
      buf, sizeof(buf),
      "%02" PRIuMAX ":%02" PRIuMAX ":%02" PRIuMAX ".%03" PRIuMAX,
      static_cast<uintmax_t>(std::chrono::duration_cast<std::chrono::hours>(dur).count())%24,
      static_cast<uintmax_t>(std::chrono::duration_cast<std::chrono::minutes>(dur).count())%60,
      static_cast<uintmax_t>(std::chrono::duration_cast<std::chrono::seconds>(dur).count())%60,
      static_cast<uintmax_t>(std::chrono::duration_cast<std::chrono::milliseconds>(dur).count())%1000);
  return buf;
}

}  // namespace kingtaker
