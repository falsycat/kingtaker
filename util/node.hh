#pragma once

#include "kingtaker.hh"

#include <cassert>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

#include <imgui.h>
#include <msgpack.hh>

#include "iface/node.hh"

#include "util/gui.hh"
#include "util/history.hh"
#include "util/node_logger.hh"
#include "util/ptr_selector.hh"
#include "util/value.hh"


namespace kingtaker {

// An object that stores links between nodes.
class NodeLinkStore final {
 public:
  class SwapCommand;

  using Node    = iface::Node;
  using Sock    = Node::Sock;
  using InSock  = Node::InSock;
  using OutSock = Node::OutSock;

  template <typename T>
  struct SockRef final {
    SockRef() = default;
    SockRef(T* s) noexcept : node(s->owner()), name(s->name()), sock(s) {
    }
    SockRef(Node* n, std::string_view na) noexcept :
        node(n), name(na), sock(nullptr) {
    }
    Node*       node;
    std::string name;
    T*          sock;
  };
  struct SockLink final {
    SockLink() = default;
    SockLink(InSock* in, OutSock* out) noexcept : in(in), out(out) {
    }
    SockRef<InSock>  in;
    SockRef<OutSock> out;
  };

  NodeLinkStore() = default;
  NodeLinkStore(const NodeLinkStore&) = delete;
  NodeLinkStore(NodeLinkStore&&) = delete;
  NodeLinkStore& operator=(const NodeLinkStore&) = delete;
  NodeLinkStore& operator=(NodeLinkStore&&) = delete;

  NodeLinkStore(const msgpack::object&, const std::vector<Node*>&);
  static std::vector<SockLink> DeserializeLinks(const msgpack::object&, const std::vector<Node*>&);
  void Serialize(
      Packer&, const std::unordered_map<Node*, size_t>& idxmap) const noexcept;
  std::unique_ptr<NodeLinkStore> Clone(const std::unordered_map<Node*, Node*>& src_to_dst) const noexcept;

  void Link(InSock* in, OutSock* out) noexcept;
  void Unlink(const InSock*, const OutSock*) noexcept;  // deleted pointers can be passed

  using DeadLinkListener = std::function<void(const SockLink&)>;
  void ListenDeadLink(DeadLinkListener&& f) noexcept {
    dead_listener_ = std::move(f);
  }

  // deleted pointers can be passed
  std::vector<OutSock*> GetSrcOf(const InSock* sock) const noexcept;
  // deleted pointers can be passed
  std::vector<InSock*> GetDstOf(const OutSock* out) const noexcept;

  const std::vector<SockLink>& items() const noexcept { return items_; }

 private:
  DeadLinkListener dead_listener_;

  std::vector<SockLink> items_;

  class Observer;
  std::unordered_map<Node*, std::unique_ptr<Node::Observer>> obs_;


  NodeLinkStore(std::vector<SockLink>&& items) noexcept;
};

class NodeLinkStore::SwapCommand : public HistoryCommand {
 public:
  enum Type { kLink, kUnlink, };

  SwapCommand(NodeLinkStore* links, Type t, const InSock& in, const OutSock& out) noexcept :
      SwapCommand(links, t, in.owner(), in.name(), out.owner(), out.name()) {
  }
  SwapCommand(NodeLinkStore* links, Type t, const SockLink& link) noexcept :
      SwapCommand(links, t, link.in.node, link.in.name, link.out.node, link.out.name) {
  }
  SwapCommand(NodeLinkStore* links, Type t,
              Node* in_node,  std::string_view in_name,
              Node* out_node, std::string_view out_name) noexcept :
      links_(links), type_(t),
      in_node_(in_node), in_name_(in_name),
      out_node_(out_node), out_name_(out_name) {
  }

  void Apply() override {
    switch (type_) {
    case kLink  : Link();   break;
    case kUnlink: Unlink(); break;
    }
  }
  void Revert() override {
    switch (type_) {
    case kLink  : Unlink(); break;
    case kUnlink: Link();   break;
    }
  }

 private:
  NodeLinkStore* links_;

  Type type_;

  Node*       in_node_;
  std::string in_name_;
  Node*       out_node_;
  std::string out_name_;

  void Link() const {
    auto in  = in_node_->in(in_name_);
    auto out = out_node_->out(out_name_);
    if (!in || !out) throw Exception("cannot link deleted sockets");
    links_->Link(in, out);
  }
  void Unlink() const {
    auto in  = in_node_->in(in_name_);
    auto out = out_node_->out(out_name_);
    if (!in || !out) throw Exception("cannot unlink deleted sockets");
    links_->Unlink(in, out);
  }
};


// An implementation of Context that redirects an output of target node to a
// specific socket.
class NodeRedirectContext : public iface::Node::Context {
 public:
  using Node = iface::Node;

  NodeRedirectContext(File::Path&&                          path,
                      const std::shared_ptr<Node::Context>& octx,
                      const std::weak_ptr<Node::OutSock>&   odst,
                      Node*                                 target = nullptr) noexcept :
      Context(std::move(path), octx), odst_(odst), target_(target) {
  }

  void Attach(Node* target) noexcept {
    target_ = target;
  }
  void ObserveSend(const Node::OutSock& src, const Value& v) noexcept override {
    if (src.owner() != target_) return;
    auto odst = odst_.lock();
    if (odst) odst->Send(octx(), Value::Tuple { src.name(), Value(v) });
  }

  Node* target() const noexcept { return target_; }

 private:
  std::weak_ptr<Node::OutSock> odst_;

  Node* target_;
};


// An implemetation of InSock that executes lambda when received something
class NodeLambdaInSock final : public iface::Node::InSock {
 public:
  using Node     = iface::Node;
  using Receiver = std::function<void(const std::shared_ptr<Node::Context>&, Value&&)>;

  NodeLambdaInSock(Node* o, std::string_view name, Receiver&& f) noexcept :
      InSock(o, name), lambda_(std::move(f)) {
  }

  void Receive(const std::shared_ptr<Node::Context>& ctx, Value&& v) noexcept override {
    lambda_(ctx, std::move(v));
  }

 private:
  Receiver lambda_;
};


// Use with LambdaNode<Driver>.
class LambdaNodeDriver : public iface::Node::Context::Data {
 public:
  using TypeInfo = File::TypeInfo;
  using Path     = File::Path;
  using Node     = iface::Node;
  using InSock   = Node::InSock;
  using OutSock  = Node::OutSock;
  using Context  = Node::Context;

  // static constexpr char* kTitle = "";

  using SockMeta = std::pair<std::string, std::string>;
  // static inline const std::vector<SockMeta> kInSocks;
  // static inline const std::vector<SockMeta> kOutSocks;

  LambdaNodeDriver() = default;
  // LambdaNodeDriver(LambdaNode*, const std::weak_ptr<Context>&);
  LambdaNodeDriver(const LambdaNodeDriver&) = default;
  LambdaNodeDriver(LambdaNodeDriver&&) = default;
  LambdaNodeDriver& operator=(const LambdaNodeDriver&) = default;
  LambdaNodeDriver& operator=(LambdaNodeDriver&&) = default;

  // std::string title() const noexcept;

  // void Handle(size_t, Value&&);
};

template <typename Driver>
class LambdaNode final : public File, public iface::Node {
 public:
  LambdaNode(Env* env) noexcept :
      File(&Driver::kType, env), Node(Node::kNone),
      in_insts_(Driver::kInSocks.size()), out_insts_(Driver::kOutSocks.size()) {
    out_.reserve(Driver::kOutSocks.size());
    for (size_t i = 0; i < Driver::kOutSocks.size(); ++i) {
      const auto& m = Driver::kOutSocks[i];
      out_insts_[i] = std::make_shared<OutSock>(this, m.first);
      out_.push_back(out_insts_[i].get());
    }

    in_.reserve(Driver::kInSocks.size());
    for (size_t i = 0; i < Driver::kInSocks.size(); ++i) {
      const auto& m = Driver::kInSocks[i];
      in_insts_[i].emplace(this, m.first, i);
      in_.push_back(&*in_insts_[i]);
    }
  }

  LambdaNode(Env* env, const msgpack::object&) noexcept :
      LambdaNode(env) {
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_nil();
  }
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    return std::make_unique<LambdaNode>(env);
  }

  void UpdateNode(const std::shared_ptr<Editor>&) noexcept override;

  void Initialize(const std::shared_ptr<Context>& ctx) noexcept override {
    ctx->CreateData<Driver>(this, this, std::weak_ptr<Context>(ctx));
  }

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node>(t).Select(this);
  }

  const std::shared_ptr<OutSock>& sharedOut(size_t idx) const noexcept {
    return out_insts_[idx];
  }

 private:
  class CustomInSock;
  std::vector<std::optional<CustomInSock>> in_insts_;

  std::vector<std::shared_ptr<OutSock>> out_insts_;


  class CustomInSock final : public InSock {
   public:
    CustomInSock(LambdaNode* o, std::string_view name, size_t idx) noexcept :
        InSock(o, name), owner_(o), idx_(idx) {
    }

    void Receive(const std::shared_ptr<Context>& ctx, Value&& v) noexcept override {
      try {
        ctx->data<Driver>(owner_)->Handle(idx_, std::move(v));
      } catch (Exception& e) {
        NodeLoggerTextItem::Error(
            owner_->abspath(), *ctx, "while handling input ("+name()+"), "+e.msg());
      }
    }

    LambdaNode* owner() const noexcept { return owner_; }
    size_t idx() const noexcept { return idx_; }

   private:
    LambdaNode* owner_;

    size_t idx_;
  };
};
template <typename Driver>
void LambdaNode<Driver>::UpdateNode(const std::shared_ptr<Editor>& ctx) noexcept {
  const auto driver = ctx->data<Driver>(this);
  ImGui::TextUnformatted(driver->title().c_str());

  ImGui::BeginGroup();
  for (const auto& m : Driver::kInSocks) {
    gui::NodeInSock(m.first, m.second);
  }
  ImGui::EndGroup();

  ImGui::SameLine();

  ImGui::BeginGroup();
  float w = 0;
  for (const auto& m : Driver::kOutSocks) {
    w = std::max(w, ImGui::CalcTextSize(m.first.c_str()).x);
  }
  const auto left = ImGui::GetCursorPosX();
  for (const auto& m : Driver::kOutSocks) {
    ImGui::SetCursorPosX(left+w-ImGui::CalcTextSize(m.first.c_str()).x);
    gui::NodeOutSock(m.first, m.second);
  }
  ImGui::EndGroup();
}


// Saves all socket names of a specific node.
class NodeSockNameList final {
 public:
  NodeSockNameList() = default;
  NodeSockNameList(iface::Node* node) noexcept {
    in_.reserve(node->in().size());
    for (auto sock : node->in()) in_.push_back(sock->name());

    out_.reserve(node->out().size());
    for (auto sock : node->out()) out_.push_back(sock->name());
  }
  NodeSockNameList(const NodeSockNameList&) = default;
  NodeSockNameList(NodeSockNameList&&) = default;
  NodeSockNameList& operator=(const NodeSockNameList&) = default;
  NodeSockNameList& operator=(NodeSockNameList&&) = default;

  bool operator==(const NodeSockNameList& other) const noexcept {
    return other.in_ == in_ && other.out_ == out_;
  }
  bool operator!=(const NodeSockNameList& other) const noexcept {
    return other.in_ != in_ || other.out_ != out_;
  }

  NodeSockNameList(const msgpack::object& obj) {
    msgpack::find(obj, "in"s).convert(in_);
    msgpack::find(obj, "out"s).convert(out_);
  }
  void Serialize(Packer& pk) const noexcept {
    pk.pack_map(2);

    pk.pack("in"s);
    pk.pack(in_);

    pk.pack("out"s);
    pk.pack(out_);
  }

  const std::string& in(size_t i) const noexcept { return in_[i]; }
  const std::string& out(size_t i) const noexcept { return out_[i]; }

  std::span<const std::string> in() const noexcept { return in_; }
  std::span<const std::string> out() const noexcept { return out_; }

 private:
  std::vector<std::string> in_;
  std::vector<std::string> out_;
};

}  // namespace kingtaker
