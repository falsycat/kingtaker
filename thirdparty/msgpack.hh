#pragma once

#include <string>
#include <string_view>
#include <type_traits>

#include <imgui.h>
#include <msgpack.hpp>

namespace msgpack {

template <typename T>
inline const object& find(const object_map& map, const T& key, const object& def) noexcept {
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
  return def;
}
template <typename T>
inline object find(const object& map, const T& key, object def) noexcept {
  return map.type == type::MAP? find(map.via.map, key, def): def;
}
template <typename T1, typename T2>
inline object find(const object& map, const T1& key, T2 def) noexcept {
  return find(map, key, object {def});
}
template <typename T>
inline const object& find(const object& map, const T& key) noexcept {
  static const object nil_;
  return map.type == type::MAP? find(map.via.map, key, nil_): nil_;
}

template <typename T>
inline T as_if(const object& obj, T def) {
  obj.convert_if_not_nil(def);
  return def;
}
template <>
inline ImVec2 as_if<ImVec2>(const object& obj, ImVec2 def) {
  if (obj.type == msgpack::type::NIL) return def;
  if (obj.type != msgpack::type::ARRAY) throw msgpack::type_error();
  if (obj.via.array.size != 2) throw msgpack::type_error();
  return ImVec2(obj.via.array.ptr[0].as<float>(), obj.via.array.ptr[1].as<float>());
}


MSGPACK_API_VERSION_NAMESPACE(MSGPACK_DEFAULT_API_NS) {
namespace adaptor {

template <>
struct pack<ImVec2> final {
 public:
  template <typename Stream>
  packer<Stream>& operator()(packer<Stream>& pk, ImVec2 const& v) const {
    pk.pack_array(2);
    pk.pack(v.x);
    pk.pack(v.y);
    return pk;
  }
};

}}  // namespace adaptor
}  // namespace msgpack
