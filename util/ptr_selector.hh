#pragma once

#include <type_traits>
#include <typeindex>


template <typename... I>
struct PtrSelector final {
 public:
  PtrSelector(std::type_index idx) noexcept : idx_(idx) { }

  template <typename T1, typename... T2>
  void* Select(T1 ptr1, T2... ptr2) noexcept {
    auto ptr = Get<T1, I...>(ptr1);
    return ptr? ptr: Select(ptr2...);
  }
  void* Select() noexcept { return nullptr; }

 private:
  template <typename T, typename I1, typename... I2>
  void* Get(T ptr) const noexcept {
    if constexpr (std::is_base_of<I1, std::remove_pointer_t<T>>::value) {
      if (idx_ == typeid(I1)) return static_cast<I1*>(ptr);
    }
    return Get<T, I2...>(ptr);
  }
  template <typename T>
  void* Get(T) const noexcept { return nullptr; }

  std::type_index idx_;
};


