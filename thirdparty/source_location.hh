#pragma once

#if defined(__clang__)
# include "source_location/source_location.hpp"
  namespace std { using source_location = ::nostd::source_location; }
#else
# include <source_location>
#endif
