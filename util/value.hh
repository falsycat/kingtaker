#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <linalg.hh>

#include "kingtaker.hh"


namespace kingtaker {

class Value final {
 public:
  using Pulse   = std::monostate;
  using Integer = int64_t;
  using Scalar  = double;
  using Boolean = bool;
  using String  = std::string;
  using Vec2    = linalg::double2;
  using Vec3    = linalg::double3;
  using Vec4    = linalg::double4;
  class Tensor;

  using Variant = std::variant<
      Pulse,
      Integer,
      Scalar,
      Boolean,
      Vec2,
      Vec3,
      Vec4,
      std::shared_ptr<String>,
      std::shared_ptr<Tensor>>;

  Value() noexcept : Value(Pulse()) { }
  Value(Pulse v) noexcept : v_(v) { }
  Value(Integer v) noexcept : v_(v) { }
  Value(Scalar v) noexcept : v_(v) { }
  Value(Boolean v) noexcept : v_(v) { }

  Value(const Vec2& v) noexcept : v_(v) { }
  Value(const Vec3& v) noexcept : v_(v) { }
  Value(const Vec4& v) noexcept : v_(v) { }

  Value(const char* v) noexcept : Value(std::string(v)) { }
  Value(String&& v) noexcept : v_(std::make_shared<String>(std::move(v))) { }
  Value(const String& v) noexcept : v_(std::make_shared<String>(v)) { }
  Value(const std::shared_ptr<String>& v) noexcept : v_(v) { }

  Value(Tensor&& t) noexcept : v_(std::make_shared<Tensor>(std::move(t))) { }

  Value(const Value&) = default;
  Value(Value&&) = default;
  Value& operator=(const Value&) = default;
  Value& operator=(Value&&) = default;

  void Serialize(File::Packer&) const;
  static Value Deserialize(const msgpack::object&);

  template <typename T>
  T& getUniq() {
    if (!has<T>()) throw Exception("incompatible Value type");

    if constexpr (IsSharedType<T>) {
      return *getUniq<std::shared_ptr<T>>();

    } else if constexpr (IsSharedPtr<T>) {
      auto& ptr = std::get<T>(v_);
      if (1 != ptr.use_count()) v_ = std::make_unique<T::element_type>(*ptr);
      return std::get<T>(v_);

    } else {
      return std::get<T>(v_);
    }
  }

  template <typename T>
  const T& get() const {
    if (!has<T>()) throw Exception("incompatible Value type");

    if constexpr (IsSharedType<T>) {
      return *get<std::shared_ptr<T>>();
    } else {
      return std::get<T>(v_);
    }
  }
  template <typename T>
  bool has() const noexcept {
    return std::holds_alternative<RawType<T>>(v_);
  }

 private:
  template <typename T>
  static constexpr auto RawType_() noexcept {
    if constexpr (std::is_same<String, T>::value) {
      return std::shared_ptr<String>();
    } else if constexpr (std::is_same<Tensor, T>::value) {
      return std::shared_ptr<Tensor>();
    } else {
      return T();
    }
  }
  template <typename T>
  using RawType = decltype(RawType_<T>());

  template <typename T>
  static constexpr bool IsSharedType = !std::is_same<RawType<T>, T>::value;

  template <typename T>
  static constexpr auto IsSharedPtr_(int) noexcept -> decltype(T::element_type, bool()) { return true; }
  template <typename T>
  static constexpr auto IsSharedPtr_(int) noexcept -> bool { return false; }
  template <typename T>
  static constexpr bool IsSharedPtr = IsSharedPtr_<T>(0);

  Variant v_;
};

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

  static std::string_view StringifyType(Type) noexcept;
  static Type ParseType(std::string_view);
  static size_t CountSamples(const std::vector<size_t>&);
  static Tensor Deserialize(const msgpack::object& obj);

  Tensor() = delete;
  Tensor(Type t, const std::vector<size_t>& d) noexcept : Tensor(t, std::vector<size_t>(d)) { }
  Tensor(Type t, std::vector<size_t>&& d) noexcept : Tensor(t, std::move(d), {}) { }
  Tensor(Type, std::vector<size_t>&&, std::vector<uint8_t>&&) noexcept;
  Tensor(const Tensor&) = default;
  Tensor(Tensor&&) = default;
  Tensor& operator=(const Tensor&) = default;
  Tensor& operator=(Tensor&&) = default;

  void Serialize(File::Packer&) const noexcept;

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

}  // namespace kingtaker
