#pragma once

#include <chrono>
#include <cstring>
#include <cinttypes>

#include "kingtaker.hh"


namespace kingtaker {

static inline std::string StringifyTime(File::Time t) noexcept {
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

template <typename R, typename P>
static inline std::string StringifyDuration(std::chrono::duration<R, P> dur) noexcept {
  const auto h  = std::chrono::duration_cast<std::chrono::hours>(dur).count();
  const auto m  = std::chrono::duration_cast<std::chrono::minutes>(dur).count()%60;
  if (h) {
    return std::to_string(h)+"h"+std::to_string(m)+"m";
  }

  const auto s  = std::chrono::duration_cast<std::chrono::seconds>(dur).count()%60;
  if (m) {
    return std::to_string(m)+"m"+std::to_string(s)+"s";
  }

  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
  return std::to_string(static_cast<double>(ms)/1000.)+"s";
}

}  // namespace kingtaker
