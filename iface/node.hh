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
  class Context;
  class Editor;

  struct SockMeta;
  class  InSock;
  class  OutSock;

  enum Flag : uint8_t {
    kNone = 0,
    kMenu = 0b1,
  };
  using Flags = uint8_t;

  Node(Flags f) noexcept : flags_(f) {
  }
  virtual ~Node() = default;
  Node(const Node&) = delete;
  Node(Node&&) = delete;
  Node& operator=(const Node&) = delete;
  Node& operator=(Node&&) = delete;

  virtual void UpdateNode(File::RefStack&, const std::shared_ptr<Editor>&) noexcept { }
  virtual void UpdateMenu(File::RefStack&, const std::shared_ptr<Editor>&) noexcept { }

  std::span<const std::shared_ptr<InSock>> in() const noexcept { return in_; }
  std::span<const std::shared_ptr<OutSock>> out() const noexcept { return out_; }

  const std::shared_ptr<InSock>& in(size_t i) const noexcept { return in_[i]; }
  const std::shared_ptr<OutSock>& out(size_t i) const noexcept { return out_[i]; }

  inline std::shared_ptr<InSock> in(std::string_view) const noexcept;
  inline std::shared_ptr<OutSock> out(std::string_view) const noexcept;

  Flags flags() const noexcept { return flags_; }

 protected:
  std::vector<std::shared_ptr<InSock>> in_;
  std::vector<std::shared_ptr<OutSock>> out_;

 private:
  Flags flags_;
};

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

  // Returns an empty when the socket is destructed or missing.
  virtual std::span<const std::shared_ptr<InSock>> dstOf(const OutSock*) const noexcept {
    return {};
  }
  virtual std::span<const std::shared_ptr<OutSock>> srcOf(const InSock*) const noexcept {
    return {};
  }

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

  virtual void Link(const std::shared_ptr<InSock>&,
                    const std::shared_ptr<OutSock>&) noexcept = 0;

  virtual void Unlink(const InSock&, const OutSock&) noexcept = 0;

  void Unlink(const InSock& in) noexcept {
    const auto src_span = srcOf(&in);
    std::vector<std::shared_ptr<OutSock>> srcs(src_span.begin(), src_span.end());
    for (const auto& out : srcs) Unlink(in, *out);
  }
  void Unlink(const OutSock& out) noexcept {
    const auto dst_span = dstOf(&out);
    std::vector<std::shared_ptr<InSock>> dsts(dst_span.begin(), dst_span.end());
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
    kString,
    kStringMultiline,
    kStringOption,
    kStringPath,
    kTuple,
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

  // when type == kTuple
  std::vector<std::shared_ptr<SockMeta>> tupleFields = {};

  // when type == kData
  std::string dataType = "";


  static std::shared_ptr<SockMeta> shared(SockMeta&& meta) noexcept {
    auto ret = std::make_shared<SockMeta>();
    *ret = std::move(meta);
    return ret;
  }
  std::shared_ptr<const SockMeta> gshared() const noexcept {
    return {this, [](auto){}};
  }
};

class Node::InSock {
 public:
  InSock(Node* o, const std::shared_ptr<const SockMeta>& meta) noexcept :
      owner_(o), meta_(meta) {
  }
  virtual ~InSock() = default;
  InSock(const InSock&) = delete;
  InSock(InSock&&) = delete;
  InSock& operator=(const InSock&) = delete;
  InSock& operator=(InSock&&) = delete;

  virtual void Receive(const std::shared_ptr<Context>&, Value&&) noexcept { }

  /* it's possible that the owner dies */
  Node* owner() const noexcept { return owner_; }
  const std::shared_ptr<const SockMeta>& meta() const noexcept { return meta_; }
  const std::string& name() const noexcept { return meta_->name; }

 private:
  Node* owner_;

  std::shared_ptr<const SockMeta> meta_;
};

class Node::OutSock {
 public:
  OutSock(Node* o, const std::shared_ptr<const SockMeta>& meta) noexcept :
      owner_(o), meta_(meta) {
  }
  virtual ~OutSock() = default;
  OutSock(const OutSock&) = delete;
  OutSock(OutSock&&) = delete;
  OutSock& operator=(const OutSock&) = delete;
  OutSock& operator=(OutSock&&) = delete;

  void Send(const std::shared_ptr<Context>& ctx, Value&& v) noexcept {
    ctx->ObserveSend(*this, v);

    auto task = [self = this, ctx, v = std::move(v)]() mutable {
      // self may be already destructed
      const auto dst = ctx->dstOf(self);
      for (const auto& other : dst) {
        ctx->ObserveReceive(*other, v);
        other->Receive(ctx, Value(v));
      }
    };
    Queue::sub().Push(std::move(task));
  }

  /* it's possible that the owner is dead */
  Node* owner() const noexcept { return owner_; }
  const std::shared_ptr<const SockMeta>& meta() const noexcept { return meta_; }
  const std::string& name() const noexcept { return meta_->name; }

 private:
  Node* owner_;

  std::shared_ptr<const SockMeta> meta_;
};

std::shared_ptr<Node::InSock> Node::in(std::string_view name) const noexcept {
  for (const auto& sock : in_) {
    if (sock->name() == name) return sock;
  }
  return nullptr;
}
std::shared_ptr<Node::OutSock> Node::out(std::string_view name) const noexcept {
  for (const auto& sock : out_) {
    if (sock->name() == name) return sock;
  }
  return nullptr;
}

}  // namespace kingtaker::iface
