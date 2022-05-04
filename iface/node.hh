#pragma once

#include <algorithm>
#include <cassert>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "kingtaker.hh"

#include "util/value.hh"


namespace kingtaker::iface {

class Node {
 public:
  class Observer;

  class Context;
  class Editor;

  struct SockMeta;
  class  Sock;
  class  InSock;
  class  OutSock;

  enum Flag : uint8_t {
    kNone = 0,
    kMenu = 0b1,
  };
  using Flags = uint8_t;

  Node(Flags f) noexcept : flags_(f) {
  }
  virtual inline ~Node() noexcept;
  Node(const Node&) = delete;
  Node(Node&&) = delete;
  Node& operator=(const Node&) = delete;
  Node& operator=(Node&&) = delete;

  virtual void Update(const std::shared_ptr<Editor>&) noexcept { }
  virtual void UpdateNode(const std::shared_ptr<Editor>&) noexcept { }
  virtual void UpdateMenu(const std::shared_ptr<Editor>&) noexcept { }

  std::span<InSock* const> in() const noexcept { return in_; }
  std::span<OutSock* const> out() const noexcept { return out_; }

  InSock& in(size_t i) const noexcept { return *in_[i]; }
  OutSock& out(size_t i) const noexcept { return *out_[i]; }

  inline InSock* in(std::string_view) const noexcept;
  inline OutSock* out(std::string_view) const noexcept;

  Flags flags() const noexcept { return flags_; }

 protected:
  std::vector<InSock*> in_;
  std::vector<OutSock*> out_;

  // after modifying in_ or out_, call this.
  inline void NotifySockChange() const noexcept;

 private:
  std::vector<Observer*> obs_;

  Flags flags_;
};

class Node::Observer {
 public:
  Observer() = delete;
  Observer(Node* target) noexcept : target_(target) {
    target_->obs_.push_back(this);
  }
  virtual ~Observer() noexcept {
    if (!target_) return;

    auto& obs = target_->obs_;
    auto  itr = std::find(obs.begin(), obs.end(), this);
    if (itr != obs.end()) obs.erase(itr);
  }
  Observer(const Observer&) = delete;
  Observer(Observer&&) = delete;
  Observer& operator=(const Observer&) = delete;
  Observer& operator=(Observer&&) = delete;

  virtual void ObserveSockChange() noexcept { }

  // Do not forget to call this implementation of base class when override.
  virtual void ObserveDie() noexcept { target_ = nullptr; }

  Node* target() const noexcept { return target_; }

 private:
  Node* target_;
};
Node::~Node() noexcept {
  for (size_t i = 0; i < obs_.size(); ++i) {
    obs_[obs_.size()-i-1]->ObserveDie();
  }
}
void Node::NotifySockChange() const noexcept {
  for (size_t i = 0; i < obs_.size(); ++i) {
    obs_[obs_.size()-i-1]->ObserveSockChange();
  }
}

class Node::Context {
 public:
  class Data {
   public:
    Data() = default;
    virtual ~Data() = default;
  };

  Context() = delete;
  Context(File::Path&& basepath) noexcept : basepath_(std::move(basepath)) {
  }
  virtual ~Context() = default;
  Context(const Context&) = delete;
  Context(Context&&) = delete;
  Context& operator=(const Context&) = delete;
  Context& operator=(Context&&) = delete;

  virtual void ObserveReceive(const InSock&, const Value&) noexcept { }

  // must be thread-safe
  virtual void ObserveSend(const OutSock&, const Value&) noexcept { }
  virtual void Notify(File*, std::string_view) noexcept { }

  // Returns an empty when the socket is destructed or missing.
  virtual std::vector<InSock*> dstOf(const OutSock*) const noexcept { return {}; }
  virtual std::vector<OutSock*> srcOf(const InSock*) const noexcept { return {}; }

  const File::Path& basepath() const noexcept { return basepath_; }

  template <typename T, typename... Args>
  std::shared_ptr<T> data(Node* n, Args... args) noexcept {
    auto [itr, created] = data_.try_emplace(n);
    if (!created) {
      auto ptr = std::dynamic_pointer_cast<T>(itr->second);
      if (ptr) return ptr;
    }
    auto ret = std::make_shared<T>(std::forward<Args>(args)...);
    itr->second = ret;
    return ret;
  }

 private:
  File::Path basepath_;

  std::unordered_map<Node*, std::shared_ptr<Data>> data_;
};

class Node::Editor : public Context {
 public:
  Editor() = delete;
  Editor(File::Path&& basepath) noexcept : Context(std::move(basepath)) {
  }
  Editor(const Editor&) = delete;
  Editor(Editor&&) = delete;
  Editor& operator=(const Editor&) = delete;
  Editor& operator=(Editor&&) = delete;

  virtual void Link(const InSock&, const OutSock&) noexcept = 0;

  virtual void Unlink(const InSock&, const OutSock&) noexcept = 0;

  void Unlink(const InSock& in) noexcept {
    const auto src_span = srcOf(&in);
    std::vector<OutSock*> srcs(src_span.begin(), src_span.end());
    for (const auto& out : srcs) Unlink(in, *out);
  }
  void Unlink(const OutSock& out) noexcept {
    const auto dst_span = dstOf(&out);
    std::vector<InSock*> dsts(dst_span.begin(), dst_span.end());
    for (const auto& in : dsts) Unlink(*in, out);
  }
};

struct Node::SockMeta final {
 public:
  enum Type {
    kAny,
    kPulse,
    kInteger,
    kScalar,
    kVec2,
    kVec3,
    kVec4,
    kScalarNormal,
    kVec2Normal,
    kVec3Normal,
    kVec4Normal,
    kNamedValue,
    kString,
    kStringMultiline,
    kStringOption,
    kStringPath,
    kData,
  };

  std::string name;  // must be unique in the node
  Type        type = kAny;
  std::string desc = "";

  // flags
  unsigned trigger : 1 = false;
  unsigned multi   : 1 = false;

  // default value
  std::optional<Value> def = std::nullopt;

  // when type == kStringOption
  std::vector<Value::String> stringOptions = {};

  // when type == kData
  std::string dataType = "";
};

class Node::Sock {
 public:
  Sock(Node* o, const SockMeta* meta) noexcept : owner_(o), meta_(meta) {
  }
  virtual ~Sock() = default;
  Sock(const Sock&) = delete;
  Sock(Sock&&) = delete;
  Sock& operator=(const Sock&) = delete;
  Sock& operator=(Sock&&) = delete;

  Node* owner() const noexcept { return owner_; }
  const SockMeta& meta() const noexcept { return *meta_; }
  const std::string& name() const noexcept { return meta_->name; }

 private:
  Node* owner_;

  const SockMeta* meta_;
};

class Node::InSock : public Sock {
 public:
  InSock(Node* o, const SockMeta* meta) noexcept : Sock(o, meta) {
  }
  virtual void Receive(const std::shared_ptr<Context>&, Value&&) noexcept { }
};

class Node::OutSock : public Sock {
 public:
  OutSock(Node* o, const SockMeta* meta) noexcept : Sock(o, meta) {
  }

  // thread-safe
  void Send(const std::shared_ptr<Context>& ctx, Value&& v) noexcept {
    ctx->ObserveSend(*this, v);

    auto task = [self = this, ctx, v = std::move(v)]() {
      // self may be destructed already but dstOf can take invalid pointer
      const auto dst = ctx->dstOf(self);
      for (const auto& other : dst) {
        ctx->ObserveReceive(*other, v);
        other->Receive(ctx, Value(v));
      }
    };
    Queue::sub().Push(std::move(task));
  }
};

Node::InSock* Node::in(std::string_view name) const noexcept {
  for (const auto& sock : in_) {
    if (sock->name() == name) return sock;
  }
  return nullptr;
}
Node::OutSock* Node::out(std::string_view name) const noexcept {
  for (const auto& sock : out_) {
    if (sock->name() == name) return sock;
  }
  return nullptr;
}

}  // namespace kingtaker::iface
