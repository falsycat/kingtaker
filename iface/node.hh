#pragma once

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

#include "util/value.hh"

#include "kingtaker.hh"


namespace kingtaker::iface {

class Node {
 public:
  class Context;

  class Sock;
  class InSock;
  class CachedInSock;
  class LambdaInSock;
  class OutSock;

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

  virtual void Update(File::RefStack&, const std::shared_ptr<Context>&) noexcept { }
  virtual void UpdateMenu(File::RefStack&, const std::shared_ptr<Context>&) noexcept { }

  inline std::shared_ptr<InSock> FindIn(std::string_view) const noexcept;
  inline std::shared_ptr<OutSock> FindOut(std::string_view) const noexcept;

  std::span<const std::shared_ptr<InSock>> in() noexcept { return in_; }
  std::span<const std::shared_ptr<OutSock>> out() noexcept { return out_; }

  Flags flags() const noexcept { return flags_; }

 protected:
  InSockList  in_;
  OutSockList out_;

 private:
  Flags flags_;
};


// context for Node execution
class Node::Context {
 public:
  class Data {
   public:
    Data() = default;
    virtual ~Data() = default;
    Data(const Data&) = default;
    Data(Data&&) = default;
    Data& operator=(const Data&) = default;
    Data& operator=(Data&&) = default;
  };

  Context() = default;
  virtual ~Context() = default;
  Context(const Context&) = delete;
  Context(Context&&) = delete;
  Context& operator=(const Context&) = delete;
  Context& operator=(Context&&) = delete;

  virtual void ObserveSend(const OutSock&, const Value&) noexcept { }

  template <typename T, typename... Args>
  std::shared_ptr<T> GetOrNew(Node* n, Args... args) noexcept {
    auto [itr, created] = map_.try_emplace(n);
    if (!created) {
      auto ptr = std::dynamic_pointer_cast<T>(itr->second);
      if (ptr) return ptr;
    }
    auto ret = std::make_shared<T>(args...);
    itr->second = ret;
    return ret;
  }

 private:
  std::unordered_map<Node*, std::shared_ptr<Data>> map_;
};


// base of all Node sockets
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

// A Node socket that receive values from output sockets
// all operations must be done from main thread
class Node::InSock : public Sock {
 public:
  friend Node;

  InSock(Node* o, std::string_view n) noexcept : Sock(o, n) {
  }

  virtual void Receive(const std::shared_ptr<Context>&, Value&&) noexcept { }

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

  void Receive(const std::shared_ptr<Context>&, Value&& v) noexcept override {
    value_ = std::move(v);
  }

  const Value& value() const noexcept { return value_; }

 private:
  Value value_;
};
class Node::LambdaInSock final : public InSock {
 public:
  using Receiver = std::function<void(const std::shared_ptr<Context>&, Value&&)>;

  LambdaInSock(Node* o, std::string_view n, Receiver&& f) noexcept :
      InSock(o, n), lambda_(std::move(f)) {
  }

  void Receive(const std::shared_ptr<Context>& ctx, Value&& v) noexcept override {
    lambda_(ctx, std::move(v));
  }
 private:
  Receiver lambda_;
};


// A Node socket that emits value to input sockets
// all operations must be done from main thread
class Node::OutSock : public Sock {
 public:
  friend Node;

  OutSock(Node* o, std::string_view n) noexcept : Sock(o, n) {
  }

  virtual void Send(const std::shared_ptr<Context>& ctx, Value&& v) noexcept {
    ctx->ObserveSend(*this, v);
    for (auto& dst : dst_) {
      auto ptr = dst.lock();
      if (ptr) ptr->Receive(ctx, Value(v));
    }
  }

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


bool Node::Link(const std::weak_ptr<OutSock>& out,
                const std::weak_ptr<InSock>&  in) noexcept {
  auto s_in  = in.lock();
  auto s_out = out.lock();
  if (!s_in || !s_out) return false;

  s_in->src_.emplace_back(out);
  s_out->dst_.emplace_back(in);
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
