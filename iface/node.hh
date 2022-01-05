#pragma once

#include "kingtaker.hh"

#include <algorithm>
#include <cassert>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <msgpack.hh>


namespace kingtaker::iface {

class Node {
 public:
  using Pulse   = std::monostate;
  using Integer = int64_t;
  using Scalar  = double;
  using Boolean = bool;
  using String  = std::string;

  class Value;

  class Sock;
  class InSock;
  class CachedInSock;
  class OutSock;
  class CachedOutSock;

  using InSockList  = std::vector<std::shared_ptr<InSock>>;
  using OutSockList = std::vector<std::shared_ptr<OutSock>>;

  enum Flag : uint8_t {
    kNone = 0,
    kMenu = 0b1,
  };
  using Flags = uint8_t;

  static inline bool Link(const std::weak_ptr<OutSock>&, const std::weak_ptr<InSock>&) noexcept;
  static inline bool Unlink(const std::weak_ptr<OutSock>&, const std::weak_ptr<InSock>&) noexcept;

  Node(Flags f, InSockList&& in = {}, OutSockList&& out = {}) :
      in_(std::move(in)), out_(std::move(out)), flags_(f) {
  }
  virtual ~Node() = default;
  Node(const Node&) = delete;
  Node(Node&&) = delete;
  Node& operator=(const Node&) = delete;
  Node& operator=(Node&&) = delete;

  virtual void Update(File::RefStack&) noexcept { }
  virtual void UpdateMenu(File::RefStack&) noexcept { }

  inline std::shared_ptr<InSock> FindIn(std::string_view) const noexcept;
  inline std::shared_ptr<OutSock> FindOut(std::string_view) const noexcept;

  std::span<const std::shared_ptr<InSock>> in() noexcept { return in_; }
  std::span<const std::shared_ptr<OutSock>> out() noexcept { return out_; }

  Flags flags() const noexcept { return flags_; }

 protected:
  // don't modify from subclass except in the constructor
  InSockList  in_;
  OutSockList out_;

 private:
  Flags flags_;
};


class Node::Value final {
 public:
  Value() : Value(Pulse()) {
  }
  Value(Pulse v) : v_(std::move(v)) {
  }

  Value(Integer v) : v_(std::move(v)) {
  }

  Value(Scalar v) : v_(std::move(v)) {
  }

  Value(Boolean v) : v_(std::move(v)) {
  }

  Value(const char* v) : Value(std::string(v)) {
  }
  Value(String&& v) : v_(std::make_shared<String>(std::move(v))) {
  }

  Value(const Value&) = default;
  Value(Value&&) = default;
  Value& operator=(const Value&) = default;
  Value& operator=(Value&&) = default;

  void Serialize(File::Packer& pk) const {
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
    throw Exception("incompatible value");
  }
  static Value Deserialize(const msgpack::object& obj) {
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
    default:
      throw File::DeserializeException("incompatible value");
    }
  }

  template <typename T>
  T& getUniq() {
    if (!has<T>()) throw Exception("incompatible Value type");

    if constexpr (std::is_same<T, String>::value) {
      return *getUniq<std::shared_ptr<String>>();

    } else {
      if constexpr (std::is_same<T, std::shared_ptr<String>>::value) {
        auto ptr = std::get<T>(v_);
        if (!ptr.unique()) {
          v_ = std::make_shared<String>(*ptr);
        }
      }
      return std::get<T>(v_);
    }
  }

  template <typename T>
  const T& get() const {
    if (!has<T>()) throw Exception("incompatible Value type");

    if constexpr (std::is_same<T, String>::value) {
      return *get<std::shared_ptr<String>>();
    } else {
      return std::get<T>(v_);
    }
  }

  template <typename T>
  bool has() const noexcept {
    if constexpr (std::is_same<T, String>::value) {
      return has<std::shared_ptr<String>>();
    } else {
      return std::holds_alternative<T>(v_);
    }
  }

 private:
  std::variant<
      Pulse,
      Integer,
      Scalar,
      Boolean,
      std::shared_ptr<String>> v_;
};


class Node::Sock {
 public:
  Sock() = delete;
  Sock(Node* o, std::string_view n) noexcept : owner_(o), name_(n) {
  }
  virtual ~Sock() = default;
  Sock(const Sock&) = delete;
  Sock(Sock&&) = default;
  Sock& operator=(const Sock&) = delete;
  Sock& operator=(Sock&&) = default;

  Node& owner() const noexcept { return *owner_; }
  const std::string& name() const noexcept { return name_; }

 private:
  Node* owner_;

  std::string name_;
};

class Node::InSock : public Sock {
 public:
  friend Node;

  InSock(Node* o, std::string_view n) noexcept : Sock(o, n) {
  }

  virtual void Receive(Value&&) noexcept { }

  virtual void NotifyLink(const std::weak_ptr<OutSock>&) noexcept { }

  void CleanConns() const noexcept {
    auto& v = const_cast<decltype(src_)&>(src_);
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](auto& e) { return e.expired(); }),
            v.end());
  }

  std::span<const std::weak_ptr<OutSock>> src() const noexcept { return src_; }

 private:
  std::vector<std::weak_ptr<OutSock>> src_;
};

class Node::CachedInSock : public InSock {
 public:
  CachedInSock(Node* o, std::string_view n, Value&& v) noexcept :
      InSock(o, n), value_(std::move(v)) {
  }

  void Receive(Value&& v) noexcept override {
    value_ = std::move(v);
  }

  const Value& value() const noexcept { return value_; }

 private:
  Value value_;
};

class Node::OutSock : public Sock {
 public:
  friend Node;

  static void Link() noexcept {
  }

  OutSock(Node* o, std::string_view n) noexcept : Sock(o, n) {
  }

  virtual void Send(Value&& v) noexcept {
    File::QueueSubTask(
        [this, v = std::move(v)]() {
          for (auto& dst : dst_) {
            auto ptr = dst.lock();
            if (ptr) ptr->Receive(Value(v));
          }
        });
  }

  virtual void NotifyLink(const std::weak_ptr<InSock>&) noexcept { }

  void CleanConns() noexcept {
    auto& v = const_cast<decltype(dst_)&>(dst_);
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](auto& e) { return e.expired(); }),
            v.end());
  }

  std::span<const std::weak_ptr<InSock>> dst() const noexcept { return dst_; }

 private:
  std::vector<std::weak_ptr<InSock>> dst_;
};

class Node::CachedOutSock : public OutSock {
 public:
  CachedOutSock(Node* o, std::string_view n, Value&& v) noexcept :
      OutSock(o, n), value_(std::move(v)) {
  }

  void Send(Value&& v) noexcept override {
    value_ = std::move(v);
    OutSock::Send(Value(value_));
  }
  void NotifyLink(const std::weak_ptr<InSock>& dst) noexcept override {
    dst.lock()->Receive(Value(value_));
  }

 private:
  Value value_;
};


bool Node::Link(const std::weak_ptr<OutSock>& out,
                const std::weak_ptr<InSock>&  in) noexcept {
  auto s_in  = in.lock();
  auto s_out = out.lock();
  if (!s_in || !s_out) return false;

  s_in->src_.emplace_back(out);
  s_out->dst_.emplace_back(in);

  s_in->NotifyLink(out);
  s_out->NotifyLink(in);
  return true;
}
bool Node::Unlink(const std::weak_ptr<OutSock>& out,
                  const std::weak_ptr<InSock>&  in) noexcept {
  auto s_out = out.lock();
  auto s_in  = in.lock();
  if (!s_in || !s_out) return false;

  auto& dst = s_out->dst_;
  dst.erase(std::remove_if(dst.begin(), dst.end(),
                           [&s_in](auto& e) {
                             return e.expired() || e.lock().get() == s_in.get();
                           }),
            dst.end());
  auto& src = s_in->src_;
  src.erase(std::remove_if(src.begin(), src.end(),
                           [&s_out](auto& e) {
                             return e.expired() || e.lock().get() == s_out.get();
                           }),
            src.end());
  return true;
}

std::shared_ptr<Node::InSock> Node::FindIn(std::string_view name) const noexcept {
  auto itr = std::find_if(in_.begin(), in_.end(),
                          [name](auto e) { return e->name() == name; });
  return itr != in_.end()? *itr: nullptr;
}
std::shared_ptr<Node::OutSock> Node::FindOut(std::string_view name) const noexcept {
  auto itr = std::find_if(out_.begin(), out_.end(),
                          [name](auto e) { return e->name() == name; });
  return itr != out_.end()? *itr: nullptr;
}

}  // namespace kingtaker::iface
