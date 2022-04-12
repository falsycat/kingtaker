#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <linalg.hh>

#include "kingtaker.hh"


namespace kingtaker {

class ValueException : public Exception {
 public:
  ValueException(std::string_view msg, Loc loc = Loc::current()) noexcept :
      Exception(msg, loc) {
  }
};


class Value final {
 public:
  using Pulse   = std::monostate;
  using Integer = int64_t;
  using Scalar  = double;
  using Boolean = bool;
  using String  = std::string;
  class Tensor;
  class Data;
  class Tuple;

  using Variant = std::variant<
      Pulse,
      Integer,
      Scalar,
      Boolean,
      std::shared_ptr<String>,
      std::shared_ptr<Tensor>,
      std::shared_ptr<Data>,
      std::shared_ptr<Tuple>>;

  Value() noexcept : Value(Pulse()) { }
  Value(Pulse v) noexcept : v_(v) { }
  Value(Integer v) noexcept : v_(v) { }
  Value(Scalar v) noexcept : v_(v) { }
  Value(Boolean v) noexcept : v_(v) { }

  Value(const char* v) noexcept : Value(std::string(v)) { }
  Value(String&& v) noexcept : v_(std::make_shared<String>(std::move(v))) { }
  Value(const String& v) noexcept : v_(std::make_shared<String>(v)) { }
  Value(const std::shared_ptr<String>& v) noexcept : v_(v) { }

  Value(Tensor&& t) noexcept : v_(std::make_shared<Tensor>(std::move(t))) { }
  Value(const std::shared_ptr<Tensor>& t) noexcept : v_(t) { }
  Value(std::shared_ptr<Tensor>&& t) noexcept : v_(std::move(t)) { }

  Value(const std::shared_ptr<Data>& d) noexcept : v_(d) { }
  Value(std::shared_ptr<Data>&& d) noexcept : v_(std::move(d)) { }

  Value(Tuple&& t) noexcept : v_(std::make_shared<Tuple>(std::move(t))) { }
  Value(const std::shared_ptr<Tuple>& t) noexcept : v_(t) { }
  Value(std::shared_ptr<Tuple>&& t) noexcept : v_(std::move(t)) { }

  Value(const Value&) = default;
  Value(Value&&) = default;
  Value& operator=(const Value&) = default;
  Value& operator=(Value&&) = default;

  Value(const msgpack::object&);
  void Serialize(Packer&) const;

  const char* StringifyType() const noexcept;
  std::string Stringify(size_t max = 64) const noexcept;

  bool isPulse() const noexcept {
    return std::holds_alternative<Pulse>(v_);
  }

  bool isInteger() const noexcept {
    return std::holds_alternative<Integer>(v_);
  }
  Integer integer() const {
    if (!isInteger()) throw ValueException("expect Integer but got "s+StringifyType());
    return std::get<Integer>(v_);
  }
  Integer& integer() {
    if (!isInteger()) throw ValueException("expect Integer but got "s+StringifyType());
    return std::get<Integer>(v_);
  }
  template <typename I>
  I integer(I min = std::numeric_limits<I>::lowest(),
            I max = std::numeric_limits<I>::max()) const {
    const auto v = integer();
    if (v < min) throw ValueException("integer underflow");
    if (v > max) throw ValueException("integer overflow");
    return static_cast<I>(v);
  }

  bool isScalar() const noexcept {
    return std::holds_alternative<Scalar>(v_);
  }
  Scalar scalar() const {
    if (!isScalar()) throw ValueException("expect Scalar but got "s+StringifyType());
    return std::get<Scalar>(v_);
  }
  Scalar& scalar() {
    if (!isScalar()) throw ValueException("expect Scalar but got "s+StringifyType());
    return std::get<Scalar>(v_);
  }
  template <typename N>
  N scalar(N min = std::numeric_limits<N>::lowest(),
           N max = std::numeric_limits<N>::max()) const {
    const auto v = scalar();
    if (v < min) throw ValueException("scalar underflow");
    if (v > max) throw ValueException("scalar overflow");
    return static_cast<N>(v);
  }

  bool isBoolean() const noexcept {
    return std::holds_alternative<Boolean>(v_);
  }
  Boolean boolean() const {
    if (!isBoolean()) throw ValueException("expect Boolean but got "s+StringifyType());
    return std::get<Boolean>(v_);
  }
  Boolean& boolean() {
    if (!isBoolean()) throw ValueException("expect Boolean but got "s+StringifyType());
    return std::get<Boolean>(v_);
  }

  bool isString() const noexcept {
    return std::holds_alternative<std::shared_ptr<String>>(v_);
  }
  const String& string() const {
    if (!isString()) throw ValueException("expect String but got "s+StringifyType());
    return *std::get<std::shared_ptr<String>>(v_);
  }
  std::shared_ptr<const String> stringPtr() const {
    if (!isString()) throw ValueException("expect String but got "s+StringifyType());
    return std::get<std::shared_ptr<String>>(v_);
  }
  String& stringUniq() noexcept {
    return *stringUniqPtr();
  }
  const std::shared_ptr<String>& stringUniqPtr() {
    if (!isString()) throw ValueException("expect String but got "s+StringifyType());

    auto& ptr = std::get<std::shared_ptr<String>>(v_);
    if (ptr.use_count() != 1) {
      ptr = std::make_shared<String>(*ptr);
    }
    return ptr;
  }

  bool isTensor() const noexcept {
    return std::holds_alternative<std::shared_ptr<Tensor>>(v_);
  }
  const Tensor& tensor() const {
    if (!isTensor()) throw ValueException("expect Tensor but got "s+StringifyType());
    return *std::get<std::shared_ptr<Tensor>>(v_);
  }
  std::shared_ptr<const Tensor> tensorPtr() const {
    if (!isTensor()) throw ValueException("expect Tensor but got "s+StringifyType());
    return std::get<std::shared_ptr<Tensor>>(v_);
  }
  Tensor& tensorUniq() {
    return *tensorUniqPtr();
  }
  inline const std::shared_ptr<Tensor>& tensorUniqPtr();

  bool isData() const noexcept {
    return std::holds_alternative<std::shared_ptr<Data>>(v_);
  }
  Data& data() const {
    if (!isData()) throw ValueException("expect Data but got "s+StringifyType());
    return *std::get<std::shared_ptr<Data>>(v_);
  }
  const std::shared_ptr<Data>& dataPtr() const {
    if (!isData()) throw ValueException("expect Data but got "s+StringifyType());
    return std::get<std::shared_ptr<Data>>(v_);
  }
  template <typename T> T& data() const;
  template <typename T> std::shared_ptr<T> dataPtr() const;

  bool isTuple() const noexcept {
    return std::holds_alternative<std::shared_ptr<Tuple>>(v_);
  }
  const Tuple& tuple() const {
    if (!isTuple()) throw ValueException("expect Tuple but got "s+StringifyType());
    return *std::get<std::shared_ptr<Tuple>>(v_);
  }
  std::shared_ptr<const Tuple> tuplePtr() const {
    if (!isTuple()) throw ValueException("expect Tuple but got "s+StringifyType());
    return std::get<std::shared_ptr<Tuple>>(v_);
  }
  Tuple& tupleUniq() {
    return *tupleUniqPtr();
  }
  inline const std::shared_ptr<Tuple>& tupleUniqPtr();
  inline const Tuple& tuple(size_t n) const;

 private:
  Variant v_;
};
bool operator==(const Value& a, const Value& b) noexcept;


class Value::Tensor final {
 public:
  enum Type : uint16_t {
    I8   = 0x0008,
    I16  = 0x0010,
    I32  = 0x0020,
    I64  = 0x0040,
    U8   = 0x0108,
    U16  = 0x0110,
    U32  = 0x0120,
    U64  = 0x0140,
    F16  = 0x0210,
    F32  = 0x0220,
    F64  = 0x0240,
  };

  class TypeUnmatchException : public Exception {
   public:
    TypeUnmatchException(Type ex, Type ac) noexcept :
        Exception(std::string(StringifyType(ex))+
                  " is expected as a tensor type but got "s+
                  std::string(StringifyType(ac))) { }
  };

  template <typename T> struct GetTypeOf;

  static const char* StringifyType(Type) noexcept;
  static Type ParseType(std::string_view);
  static size_t CountSamples(std::span<size_t>);

  Tensor() = delete;
  Tensor(Type t, const std::vector<size_t>& d) : Tensor(t, std::vector<size_t>(d)) { }
  Tensor(Type t, std::vector<size_t>&& d) : Tensor(t, std::move(d), {}) { }
  Tensor(Type, std::vector<size_t>&&, std::vector<uint8_t>&&);
  Tensor(const Tensor&) = default;
  Tensor(Tensor&&) = default;
  Tensor& operator=(const Tensor&) = default;
  Tensor& operator=(Tensor&&) = default;

  Tensor(const msgpack::object&);
  void Serialize(Packer&) const noexcept;

  std::string StringifyMeta() const noexcept;

  Type type() const noexcept { return type_; }

  template <typename T>
  std::span<T> ptr() {
    if (type_ != GetTypeOf<T>::value) {
      throw TypeUnmatchException(GetTypeOf<T>::value, type_);
    }
    return {reinterpret_cast<T*>(&buf_[0]), buf_.size()/sizeof(T)};
  }
  template <typename T>
  std::span<const T> ptr() const {
    if (type_ != GetTypeOf<T>::value) {
      throw TypeUnmatchException(GetTypeOf<T>::value, type_);
    }
    return {&buf_[0], buf_.size()/sizeof(T)};
  }

  std::span<uint8_t> ptr() noexcept { return buf_; }
  std::span<const uint8_t> ptr() const noexcept { return buf_; }

  std::span<const size_t> dim() const noexcept { return dim_; }
  size_t dim(size_t i) const noexcept { return i < dim_.size()? dim_[i]: 0; }

  size_t rank() const noexcept { return dim_.size(); }

  size_t samples() const noexcept { return buf_.size()/(type_&0xFF); }
  size_t bytes() const noexcept { return buf_.size(); }

 private:
  Type type_;
  std::vector<size_t>  dim_;
  std::vector<uint8_t> buf_;
};
template <> struct Value::Tensor::GetTypeOf<int8_t> { static constexpr Type value = I8; };
template <> struct Value::Tensor::GetTypeOf<int16_t> { static constexpr Type value = I16; };
template <> struct Value::Tensor::GetTypeOf<int32_t> { static constexpr Type value = I32; };
template <> struct Value::Tensor::GetTypeOf<int64_t> { static constexpr Type value = I64; };
template <> struct Value::Tensor::GetTypeOf<uint8_t> { static constexpr Type value = U8; };
template <> struct Value::Tensor::GetTypeOf<uint16_t> { static constexpr Type value = U16; };
template <> struct Value::Tensor::GetTypeOf<uint32_t> { static constexpr Type value = U32; };
template <> struct Value::Tensor::GetTypeOf<uint64_t> { static constexpr Type value = U64; };
template <> struct Value::Tensor::GetTypeOf<float> { static constexpr Type value = F32; };
template <> struct Value::Tensor::GetTypeOf<double> { static constexpr Type value = F64; };

const std::shared_ptr<Value::Tensor>& Value::tensorUniqPtr() {
  if (!isTensor()) throw ValueException("expect Tensor but got "s+StringifyType());

  auto& ptr = std::get<std::shared_ptr<Tensor>>(v_);
  if (ptr.use_count() != 1) {
    ptr = std::make_shared<Tensor>(*ptr);
  }
  return ptr;
}


class Value::Data {
 public:
  Data() = delete;
  Data(const char* type) noexcept : type_(type) { }
  virtual ~Data() = default;
  Data(const Data&) = default;
  Data(Data&&) = default;
  Data& operator=(const Data&) = default;
  Data& operator=(Data&&) = default;

  const char* type() const noexcept { return type_; }

 private:
  const char* type_;
};

template <typename T>
T& Value::data() const {
  static_assert(std::is_base_of<Data, T>::value, "T must be based on Value::Data");
  auto ptr = dynamic_cast<T*>(&data());
  if (!ptr) {
    throw ValueException("expect "s+typeid(T).name()+" but got "+data().type());
  }
  return *ptr;
}
template <typename T>
std::shared_ptr<T> Value::dataPtr() const {
  static_assert(std::is_base_of<Data, T>::value, "T must be based on Value::Data");
  auto ptr = std::dynamic_pointer_cast<T>(dataPtr());
  if (!ptr) {
    throw ValueException("expect "s+typeid(T).name()+" but got "+data().type());
  }
  return ptr;
}


class Value::Tuple final : public std::vector<Value> {
 public:
  using vector::vector;

  Tuple(const msgpack::object&);
  void Serialize(Packer&) const;

  const Value& operator[](size_t idx) const {
    if (idx >= size()) {
      throw ValueException("tuple index out of range"); 
    }
    return vector::operator[](idx);
  }
  Value& operator[](size_t idx) {
    if (idx >= size()) {
      throw ValueException("tuple index out of range"); 
    }
    return vector::operator[](idx);
  }

  void EnforceSize(size_t n) const {
    if (size() != n) {
      throw ValueException(
          "expected tuple size is "+std::to_string(n)+", "
          "but actually "+std::to_string(size()));
    }
  }
  std::string Stringify() const noexcept {
    std::string ret;
    for (const auto& v : *this) {
      ret += v.StringifyType() + " "s;
    }
    return ret;
  }

  linalg::float2 float2() const {
    EnforceSize(2);
    return {
      operator[](0).scalar<float>(),
      operator[](1).scalar<float>(),
    };
  }
  linalg::float3 float3() const {
    EnforceSize(3);
    return {
      operator[](0).scalar<float>(),
      operator[](1).scalar<float>(),
      operator[](2).scalar<float>(),
    };
  }
  linalg::float4 float4() const {
    EnforceSize(4);
    return {
      operator[](0).scalar<float>(),
      operator[](1).scalar<float>(),
      operator[](2).scalar<float>(),
      operator[](3).scalar<float>(),
    };
  }
};

const std::shared_ptr<Value::Tuple>& Value::tupleUniqPtr() {
  if (!isTuple()) throw ValueException("expect Tuple but got "s+StringifyType());

  auto& ptr = std::get<std::shared_ptr<Tuple>>(v_);
  if (ptr.use_count() != 1) {
    ptr = std::make_shared<Tuple>(*ptr);
  }
  return ptr;
}
const Value::Tuple& Value::tuple(size_t n) const {
  const auto& tup = tuple();
  tup.EnforceSize(n);
  return tup;
}

}  // namespace kingtaker
