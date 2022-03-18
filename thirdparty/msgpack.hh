#pragma once

#include <string>
#include <string_view>
#include <type_traits>

#include <msgpack.hpp>

namespace msgpack {

template <typename T>
inline const object& find(const object_map& map, const T& key) noexcept {
  static const object nil_;
  for (size_t i = 0; i < map.size; ++i) {
    const auto& kv = map.ptr[i];
    if constexpr (std::is_same<T, std::string>::value) {
      const auto& str  = kv.key.via.str;
      const auto  strv = std::string_view(str.ptr, str.size);
      if (kv.key.type == type::STR && key == strv) return kv.val;
    } else {
      if (kv.key == key) return kv.val;
    }
  }
  return nil_;
}
template <typename T>
inline const object& find(const object& map, const T& key) noexcept {
  static const object nil_;
  if (map.type != type::MAP) return nil_;
  return find(map.via.map, key);
}

template <typename T>
inline T as_if(const object& obj, T def) noexcept {
  obj.convert_if_not_nil(def);
  return def;
}

}  // namespace msgpack
