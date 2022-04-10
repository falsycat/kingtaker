#include "util/value.hh"

#include <msgpack.hh>


namespace kingtaker {

void Value::Serialize(File::Packer& pk) const {
  if (isInteger()) {
    pk.pack(integer());
    return;
  }
  if (isScalar()) {
    pk.pack(scalar());
    return;
  }
  if (isBoolean()) {
    pk.pack(boolean());
    return;
  }
  if (isString()) {
    pk.pack(string());
    return;
  }
  if (isTensor()) {
    auto& v = tensor();
    pk.pack_map(2);
    pk.pack("type"s);
    pk.pack("tensor"s);

    pk.pack("param"s);
    v.Serialize(pk);
    return;
  }
  throw Exception("serialization is not supported on the type");
}
Value::Value(const msgpack::object& obj)
try {
  switch (obj.type) {
  case msgpack::type::BOOLEAN:
    v_ = obj.as<Boolean>();
    return;
  case msgpack::type::POSITIVE_INTEGER:
  case msgpack::type::NEGATIVE_INTEGER:
    v_ = obj.as<Integer>();
    return;
  case msgpack::type::FLOAT:
    v_ = obj.as<float>();
    return;
  case msgpack::type::STR:
    v_ = std::make_shared<String>(obj.as<String>());
    return;

  case msgpack::type::ARRAY:
    v_ = std::make_shared<Tuple>(obj);
    return;

  case msgpack::type::MAP: {
    const auto  type  = msgpack::find(obj, "type"s).as<std::string>();
    const auto& param = msgpack::find(obj, "param"s);
    if (type == "tensor") v_ = std::make_shared<Tensor>(param);
  }
  [[fallthrough]];
  default:
    throw DeserializeException("unknown value type");
  }
} catch (msgpack::type_error&) {
  throw DeserializeException("broken Value");
}

const char* Value::StringifyType() const noexcept {
  if (isPulse())   return "pulse";
  if (isInteger()) return "integer";
  if (isScalar())  return "scalar";
  if (isBoolean()) return "boolean";
  if (isString())  return "string";
  if (isTensor())  return "tensor";
  if (isData())    return "data";
  if (isTuple())   return "tuple";
  return "unknown";
}

std::string Value::Stringify(size_t max) const noexcept {
  if (isPulse()) {
    return "Z";
  }
  if (isInteger()) {
    return std::to_string(integer());
  }
  if (isScalar()) {
    return std::to_string(scalar());
  }
  if (isBoolean()) {
    return boolean()? "T": "F";
  }
  if (isString()) {
    return string().substr(0, max);
  }
  if (isTensor()) {
    return tensor().StringifyMeta();
  }
  if (isData()) {
    return data().type();
  }
  if (isTuple()) {
    return tuple().Stringify();
  }
  return "???";
}

bool operator==(const Value& a, const Value& b) noexcept {
  if (a.isPulse() && b.isPulse()) {
    return true;
  }
  if (a.isInteger() && b.isInteger()) {
    return a.integer() == b.integer();
  }
  if (b.isScalar() && b.isScalar()) {
    return a.scalar() == b.scalar();
  }
  if (b.isBoolean() && b.isBoolean()) {
    return a.boolean() == b.boolean();
  }
  if (b.isString() && b.isString()) {
    return a.string() == b.string();
  }
  return false;
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
size_t Value::Tensor::CountSamples(std::span<size_t> dim) {
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

Value::Tensor::Tensor(Type t, std::vector<size_t>&& d, std::vector<uint8_t>&& b) :
    type_(t), dim_(std::move(d)), buf_(std::move(b)) {
  buf_.resize(CountSamples(dim_) * (t&0xFF)/8);
}

Value::Tensor::Tensor(const msgpack::object& obj)
try : Tensor(Tensor::ParseType(msgpack::find(obj, "type"s).as<std::string>()),
             msgpack::find(obj, "dim"s).as<std::vector<size_t>>(),
             msgpack::find(obj, "buf"s).as<std::vector<uint8_t>>()) {
} catch (msgpack::type_error&) {
  throw DeserializeException("broken Tensor");
} catch (Exception& e) {
  throw DeserializeException("broken Tensor: "+e.msg());
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


Value::Tuple::Tuple(const msgpack::object& obj)
try {
  for (size_t i = 0; i < obj.via.array.size; ++i) {
    emplace_back(obj.via.array.ptr[i]);
  }
} catch (msgpack::type_error&) {
  throw DeserializeException("broken Tuple");
}
void Value::Tuple::Serialize(File::Packer& pk) const {
  pk.pack_array(static_cast<uint32_t>(size()));
  for (auto& v : *this) v.Serialize(pk);
}

}  // namespace kingtaker
