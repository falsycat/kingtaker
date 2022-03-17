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
  class ContextWatcher;
  class Context;

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
  static inline bool Link(const std::shared_ptr<Context>&, const std::weak_ptr<OutSock>&, const std::weak_ptr<InSock>&) noexcept;
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


class Node::ContextWatcher {
 public:
  ContextWatcher() = default;
  virtual ~ContextWatcher() = default;
  ContextWatcher(const ContextWatcher&) = delete;
  ContextWatcher(ContextWatcher&&) = delete;
  ContextWatcher& operator=(const ContextWatcher&) = delete;
  ContextWatcher& operator=(ContextWatcher&&) = delete;

  virtual void Receive(std::string_view, Value&&) noexcept { }
};
class Node::Context final {
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

  Context(std::unique_ptr<ContextWatcher>&& w) noexcept : w_(std::move(w)) {
  }
  Context() noexcept : Context(std::make_unique<ContextWatcher>()) {
  }
  Context(const Context&) = delete;
  Context(Context&&) = delete;
  Context& operator=(const Context&) = delete;
  Context& operator=(Context&&) = delete;

  template <typename T, typename... Args>
  T& GetOrNew(Node* n, Args... args) {
    auto itr = map_.find(n);
    if (itr != map_.end()) {
      auto ptr = dynamic_cast<T*>(itr->second.get());
      if (!ptr) throw Exception("data is already set, but down cast failed");
      return *ptr;
    }
    auto uptr = std::make_unique<T>(args...);
    auto ret  = uptr.get();
    map_[n]   = std::move(uptr);
    return *ret;
  }

  void Receive(std::string_view n, Value&& v) noexcept {
    Queue::sub().Push(
        [name = std::string(n), w = w_, v = std::move(v)]() mutable {
          w->Receive(name, std::move(v));
          w = nullptr;
        });
  }

 private:
  std::shared_ptr<ContextWatcher> w_;

  std::unordered_map<Node*, std::unique_ptr<Data>> map_;
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

  virtual void Receive(const std::shared_ptr<Context>&, Value&&) noexcept { }

  virtual void NotifyLink(const std::shared_ptr<Context>&, const std::weak_ptr<OutSock>&) noexcept { }

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

class Node::OutSock : public Sock {
 public:
  friend Node;

  static void Link() noexcept {
  }

  OutSock(Node* o, std::string_view n) noexcept : Sock(o, n) {
  }

  virtual void Send(const std::shared_ptr<Context>& c, Value&& v) noexcept {
    Queue::sub().Push(
        [this, c = c, v = std::move(v)]() mutable {
          for (auto& dst : dst_) {
            auto ptr = dst.lock();
            if (ptr) ptr->Receive(c, Value(v));
          }
          c = nullptr;
        });
  }

  virtual void NotifyLink(const std::shared_ptr<Context>&, const std::weak_ptr<InSock>&) noexcept { }

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

  void Send(const std::shared_ptr<Context>& c, Value&& v) noexcept override {
    value_ = std::move(v);
    OutSock::Send(c, Value(value_));
  }
  void NotifyLink(const std::shared_ptr<Context>& c, const std::weak_ptr<InSock>& dst) noexcept override {
    dst.lock()->Receive(c, Value(value_));
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
  return true;
}
bool Node::Link(const std::shared_ptr<Context>& ctx,
                const std::weak_ptr<OutSock>& out,
                const std::weak_ptr<InSock>&  in) noexcept {
  Link(out, in);

  auto s_in  = in.lock();
  auto s_out = out.lock();
  s_in->NotifyLink(ctx, out);
  s_out->NotifyLink(ctx, in);
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
