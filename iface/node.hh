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

  Node() = delete;
  Node(std::vector<InSock*>&& in, std::vector<OutSock*>&& out) :
      in_(std::move(in)), out_(std::move(out)) { }
  virtual ~Node() = default;
  Node(const Node&) = delete;
  Node(Node&&) = delete;
  Node& operator=(const Node&) = delete;
  Node& operator=(Node&&) = delete;

  inline InSock* FindIn(std::string_view) const noexcept;
  inline OutSock* FindOut(std::string_view) const noexcept;

  std::span<InSock*> in() noexcept { return in_; }
  std::span<OutSock*> out() noexcept { return out_; }

 private:
  std::vector<InSock*> in_;
  std::vector<OutSock*> out_;
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
  friend OutSock;

  InSock(Node* o, std::string_view n) noexcept : Sock(o, n) {
  }
  inline ~InSock();

  virtual void Receive(Value&&) noexcept { }

  const std::unordered_set<OutSock*>& src() const noexcept { return src_; }

 private:
  std::unordered_set<OutSock*> src_;
};

class Node::CachedInSock : public Sock {
 public:
  CachedInSock(Node* o, std::string_view n, Value&& v) noexcept :
      Sock(o, n), value_(std::move(v)) {
  }

  const Value& value() const noexcept { return value_; }

 private:
  Value value_;
};

class Node::OutSock : public Sock {
 public:
  friend InSock;

  OutSock(Node* o, std::string_view n) noexcept : Sock(o, n) {
  }
  inline ~OutSock();

  virtual void Send(Value&& v) noexcept {
    Value v_ = std::move(v);
    File::QueueSubTask(
        [this, v_]() {
          for (auto dst : dst_) dst->Receive(Value(v_));
        });
  }

  virtual void Link(InSock& dst) noexcept {
    dst.src_.insert(this);
    dst_.insert(&dst);
  }
  void Unlink(InSock& dst) noexcept {
    dst.src_.erase(this);
    dst_.erase(&dst);
  }

  const std::unordered_set<InSock*>& dst() const noexcept { return dst_; }

 private:
  std::unordered_set<InSock*> dst_;
};

class Node::CachedOutSock : public OutSock {
 public:
  CachedOutSock(Node* o, std::string_view n, Value&& v) noexcept :
      OutSock(o, n), value_(std::move(v)) {
  }

  void Link(InSock& dst) noexcept override {
    dst.Receive(Value(value_));
    OutSock::Link(dst);
  }

 private:
  Value value_;
};


Node::InSock* Node::FindIn(std::string_view name) const noexcept {
  auto itr = std::find_if(in_.begin(), in_.end(),
                          [name](auto e) { return e->name() == name; });
  return itr != in_.end()? *itr: nullptr;
}
Node::OutSock* Node::FindOut(std::string_view name) const noexcept {
  auto itr = std::find_if(out_.begin(), out_.end(),
                          [name](auto e) { return e->name() == name; });
  return itr != out_.end()? *itr: nullptr;
}

Node::InSock::~InSock() {
  for (auto src : src_) src->dst_.erase(this);
}
Node::OutSock::~OutSock() {
  for (auto dst : dst_) dst->src_.erase(this);
}

}  // namespace kingtaker::iface
