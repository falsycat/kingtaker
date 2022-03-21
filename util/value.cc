#include "util/value.hh"

#include <msgpack.hh>


namespace kingtaker {

void Value::Serialize(File::Packer& pk) const {
  if (has<Integer>()) {
    pk.pack(get<Integer>());
    return;
  }
  if (has<Scalar>()) {
    pk.pack(get<Scalar>());
    return;
  }
  if (has<Boolean>()) {
    pk.pack(get<Boolean>());
    return;
  }
  if (has<String>()) {
    pk.pack(get<String>());
    return;
  }
  if (has<Vec2>()) {
    auto& v = get<Vec2>();
    pk.pack_array(2);
    pk.pack(v.x); pk.pack(v.y);
    return;
  }
  if (has<Vec3>()) {
    auto& v = get<Vec3>();
    pk.pack_array(3);
    pk.pack(v.x); pk.pack(v.y); pk.pack(v.z);
    return;
  }
  if (has<Vec4>()) {
    auto& v = get<Vec4>();
    pk.pack_array(4);
    pk.pack(v.x); pk.pack(v.y); pk.pack(v.z); pk.pack(v.w);
    return;
  }
  if (has<Tensor>()) {
    auto& v = get<Tensor>();
    pk.pack_map(2);
    pk.pack("type"s);
    pk.pack("tensor"s);

    pk.pack("param"s);
    v.Serialize(pk);
    return;
  }
  throw Exception("serialization is not supported on the type");
}
Value Value::Deserialize(const msgpack::object& obj) {
  try {
    switch (obj.type) {
    case msgpack::type::BOOLEAN:
      return Value(obj.via.boolean);
    case msgpack::type::POSITIVE_INTEGER:
    case msgpack::type::NEGATIVE_INTEGER:
      return Value(static_cast<Integer>(obj.via.i64));
    case msgpack::type::FLOAT:
      return Value(obj.via.f64);
    case msgpack::type::STR:
      return Value(
          std::string(obj.via.str.ptr, obj.via.str.size));
    case msgpack::type::ARRAY:
      switch (obj.via.array.size) {
      case 2: {
        const auto v = obj.as<std::array<double, 2>>();
        return Vec2(v[0], v[1]);
      }
      case 3: {
        const auto v = obj.as<std::array<double, 3>>();
        return Vec3(v[0], v[1], v[2]);
      }
      case 4: {
        const auto v = obj.as<std::array<double, 4>>();
        return Vec4(v[0], v[1], v[2], v[3]);
      } }
      break;
    case msgpack::type::MAP: {
      const auto  type  = msgpack::find(obj, "type"s).as<std::string>();
      const auto& param = msgpack::find(obj, "param"s);
      if (type == "tensor") return Tensor::Deserialize(param);
    } break;

    default:
      ;
    }
  } catch (msgpack::type_error&) {
  }
  throw DeserializeException("invalid value");
}

const char* Value::StringifyType() const noexcept {
  if (has<Pulse>())   return "pulse";
  if (has<Integer>()) return "integer";
  if (has<Scalar>())  return "scalar";
  if (has<Boolean>()) return "boolean";
  if (has<String>())  return "string";
  if (has<Vec2>())    return "vec2";
  if (has<Vec3>())    return "vec3";
  if (has<Vec4>())    return "vec4";
  if (has<Tensor>())  return "tensor";
  return "unknown";
}

std::string Value::Stringify(size_t max) const noexcept {
  if (has<Pulse>()) {
    return "Z";
  }
  if (has<Integer>()) {
    return std::to_string(get<Integer>());
  }
  if (has<Scalar>()) {
    return std::to_string(get<Scalar>());
  }
  if (has<Boolean>()) {
    return get<Boolean>()? "T": "F";
  }
  if (has<String>()) {
    return get<String>().substr(0, max);
  }
  if (has<Vec2>()) {
    auto& v = get<Vec2>();
    return "("+std::to_string(v[0])+", "+std::to_string(v[1])+")";
  }
  if (has<Vec3>()) {
    auto& v = get<Vec3>();
    return "("+std::to_string(v[0])+", "+std::to_string(v[1])+", "+std::to_string(v[2])+")";
  }
  if (has<Vec4>()) {
    auto& v = get<Vec4>();
    return "("+std::to_string(v[0])+", "+std::to_string(v[1])+", "+std::to_string(v[2])+", "+std::to_string(v[3])+")";
  }
  if (has<Tensor>()) {
    return get<Tensor>().StringifyMeta();
  }
  return "???";
}


const char* Value::Tensor::StringifyType(Type t) noexcept {
  switch (t) {
  case I8:  return "i8";
  case I16: return "i16";
  case I32: return "i32";
  case I64: return "i64";
  case U8:  return "u8";
  case U16: return "u16";
  case U32: return "u32";
  case U64: return "u64";
  case F16: return "f16";
  case F32: return "f32";
  case F64: return "f64";
  }
  return "";
}
Value::Tensor::Type Value::Tensor::ParseType(std::string_view v) {
  if (v == "i8" ) return I8;
  if (v == "i16") return I16;
  if (v == "i32") return I32;
  if (v == "i64") return I64;
  if (v == "u8" ) return U8;
  if (v == "u16") return U16;
  if (v == "u32") return U32;
  if (v == "u64") return U64;
  if (v == "f16") return F16;
  if (v == "f32") return F32;
  if (v == "f64") return F64;
  throw Exception("unknown tensor type");
}
size_t Value::Tensor::CountSamples(const std::vector<size_t>& dim) {
  if (dim.size() == 0) {
    throw Exception("empty dimension");
  }
  if (dim.end() != std::find(dim.begin(), dim.end(), 0)) {
    throw Exception("dimension has zero");
  }

  size_t n = 1;
  for (auto x : dim) {
    if (n >= SIZE_MAX/x) throw Exception("dimension overflow");
    n *= x;
  }
  return n;
}

Value::Tensor::Tensor(Type t, std::vector<size_t>&& d, std::vector<uint8_t>&& b) noexcept :
    type_(t), dim_(std::move(d)), buf_(std::move(b)) {
  buf_.resize(CountSamples(dim_) * (t&0xFF)/8);
}

Value::Tensor Value::Tensor::Deserialize(const msgpack::object& obj) {
  try {
    const auto type = Tensor::ParseType(msgpack::find(obj, "type"s).as<std::string>());

    auto dim = msgpack::find(obj, "dim"s).as<std::vector<size_t>>();
    auto buf = msgpack::find(obj, "buf"s).as<std::vector<uint8_t>>();

    CountSamples(dim);
    return Tensor(type, std::move(dim), std::move(buf));

  } catch (Exception& e) {
    throw DeserializeException("invalid tensor: "+e.msg());
  }
}
void Value::Tensor::Serialize(File::Packer& pk) const noexcept {
  pk.pack_map(3);

  pk.pack("type"s);
  pk.pack(StringifyType(type_));

  pk.pack("dim"s);
  pk.pack(dim_);

  pk.pack("buf"s);
  pk.pack(buf_);
}

std::string Value::Tensor::StringifyMeta() const noexcept {
  std::string ret = StringifyType(type_);
  ret += " ";
  for (auto& v : dim_) {
    if (ret.back() != ' ') ret += "x";
    ret += std::to_string(v);
  }
  return ret;
}

}  // namespace kingtaker
