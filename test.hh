#include <cstdlib>
#include <iostream>

#include "kingtaker.hh"


#if defined(NDEBUG) && defined(KINGTAKER_TEST)
# error "KINGTAKER_TEST is defined but NDEBUG also"
#endif


#if defined(KINGTAKER_TEST)
namespace kingtaker::test {

template <typename A, typename B>
void AssertEq(const A& a, const B& b) {
  if (a == b) return;
  std::cout << "#### ASSERTION FAILURE ####" << std::endl;
  std::abort();
}

static inline void RunAll(void) {
  AssertEq(File::ParsePath("/path/to/your/dream"),      File::Path {"path", "to", "your", "dream"});
  AssertEq(File::ParsePath("path/to/your/dream"),       File::Path {"path", "to", "your", "dream"});
  AssertEq(File::ParsePath("/path///to//your///dream"), File::Path {"path", "to", "your", "dream"});
  AssertEq(File::StringifyPath({"path", "to", "the", "hell"}), "/path/to/the/hell"s);
}

}  // namespace kingtaker::test
#endif
