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
#include <ImNodes.h>

#include "iface/node.hh"

#include "util/gui.hh"
#include "util/history.hh"
#include "util/notify.hh"
#include "util/ptr_selector.hh"
#include "util/value.hh"


namespace kingtaker {

// An object that stores links between nodes.
class NodeLinkStore final {
 public:
  class SwapCommand;

  using Node    = iface::Node;
  using InSock  = Node::InSock;
  using OutSock = Node::OutSock;

  template <typename Self, typename Other>
  struct LinkSet final {
   public:
    LinkSet() = delete;
    LinkSet(const std::shared_ptr<Self>&          self,
            std::vector<std::shared_ptr<Other>>&& others = {}) noexcept :
        self_(self), others_(std::move(others)) {
    }
    LinkSet(const LinkSet&) = delete;
    LinkSet(LinkSet&&) = default;
    LinkSet& operator=(const LinkSet&) = delete;
    LinkSet& operator=(LinkSet&&) = default;

    void Link(const std::shared_ptr<Other>& other) noexcept {
      others_.push_back(other);
    }
    bool Unlink(const Other& other) noexcept {
      for (auto itr = others_.begin(); itr < others_.end(); ++itr) {
        if (itr->get() == &other) {
          others_.erase(itr);
          return others_.empty();
        }
      }
      return false;
    }

    bool alive() const noexcept {
      return static_cast<size_t>(self_.use_count()) > others_.size();
    }

    const std::shared_ptr<Self>& self() const noexcept { return self_; }
    std::span<const std::shared_ptr<Other>> others() const noexcept { return others_; }

   private:
    std::shared_ptr<Self> self_;

    std::vector<std::shared_ptr<Other>> others_;
  };
  using InSockLinkSet  = LinkSet<InSock, OutSock>;
  using OutSockLinkSet = LinkSet<OutSock, InSock>;
  using InSockMap      = std::unordered_map<InSock*, InSockLinkSet>;
  using OutSockMap     = std::unordered_map<OutSock*, OutSockLinkSet>;

  NodeLinkStore() = default;
  NodeLinkStore(const NodeLinkStore&) = delete;
  NodeLinkStore(NodeLinkStore&&) = default;
  NodeLinkStore& operator=(const NodeLinkStore&) = delete;
  NodeLinkStore& operator=(NodeLinkStore&&) = default;

  NodeLinkStore(const msgpack::object&, const std::vector<Node*>&);
  void Serialize(
      Packer&, const std::unordered_map<Node*, size_t>& idxmap) const noexcept;
  NodeLinkStore Clone(const std::unordered_map<Node*, Node*>& src_to_dst) const noexcept;

  void Link(const std::shared_ptr<InSock>& in, const std::shared_ptr<OutSock>& out) noexcept;
  void Unlink(const InSock*, const OutSock*) noexcept;  // deleted pointers can be passed
  void CleanUp() noexcept;

  void Unlink(const InSock& in) noexcept {
    const auto src_span = srcOf(&in);
    std::vector<std::shared_ptr<OutSock>> srcs(src_span.begin(), src_span.end());
    for (const auto& out : srcs) Unlink(&in, out.get());
  }
  void Unlink(const OutSock& out) noexcept {
    const auto dst_span = dstOf(&out);
    std::vector<std::shared_ptr<InSock>> dsts(dst_span.begin(), dst_span.end());
    for (const auto& in : dsts) Unlink(in.get(), &out);
  }

  std::span<const std::shared_ptr<OutSock>> srcOf(const InSock* in) const noexcept {
    auto itr = in_.find(const_cast<InSock*>(in));
    if (itr == in_.end()) return {};
    return itr->second.others();
  }
  std::span<const std::shared_ptr<InSock>> dstOf(const OutSock* out) const noexcept {
    auto itr = out_.find(const_cast<OutSock*>(out));
    if (itr == out_.end()) return {};
    return itr->second.others();
  }

  const InSockMap&  in () const noexcept { return in_; }
  const OutSockMap& out() const noexcept { return out_; }

 private:
  InSockMap  in_;
  OutSockMap out_;

  NodeLinkStore(InSockMap&& in, OutSockMap&& out) noexcept :
      in_(std::move(in)), out_(std::move(out)) {
  }
};

class NodeLinkStore::SwapCommand : public HistoryCommand {
 public:
  enum Type { kLink, kUnlink, };

  SwapCommand(NodeLinkStore* links, Type t, const InSock& in, const OutSock& out) noexcept :
      links_(links), type_(t),
      in_node_(in.owner()), in_name_(in.name()),
      out_node_(out.owner()), out_name_(out.name()) {
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
    links_->Unlink(in.get(), out.get());
  }
};


// An impl of Context for sub context.
class NodeSubContext : public iface::Node::Context {
 public:
  using Node = iface::Node;

  NodeSubContext(const std::shared_ptr<Node::Context>& octx) noexcept :
      Context(File::Path(octx->basepath())), octx_(octx) {
  }

  std::span<const std::shared_ptr<Node::InSock>>
      dstOf(const Node::OutSock* out) const noexcept {
    return octx_->dstOf(out);
  }
  std::span<const std::shared_ptr<Node::OutSock>>
      srcOf(const Node::InSock* in) const noexcept {
    return octx_->srcOf(in);
  }

  const std::shared_ptr<Node::Context>& octx() const noexcept { return octx_; }

 private:
  std::shared_ptr<Node::Context> octx_;
};


// An implementation of Context that redirects an output of target node to a
// specific socket.
class NodeRedirectContext : public NodeSubContext {
 public:
  using Node = iface::Node;

  NodeRedirectContext(const std::shared_ptr<Node::Context>& octx,
                      const std::weak_ptr<Node::OutSock>&   odst,
                      Node*                                 target = nullptr) noexcept :
      NodeSubContext(octx), odst_(odst), target_(target) {
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

  NodeLambdaInSock(Node* o, const std::shared_ptr<const Node::SockMeta>& meta, Receiver&& f) noexcept :
      InSock(o, meta), lambda_(std::move(f)) {
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
  using RefStack = File::RefStack;
  using Path     = File::Path;
  using Node     = iface::Node;
  using SockMeta = Node::SockMeta;
  using InSock   = Node::InSock;
  using OutSock  = Node::OutSock;
  using Context  = Node::Context;

  // static constexpr char* kTitle = "";

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
      life_(std::make_shared<std::monostate>()) {
    for (size_t i = 0; i < Driver::kOutSocks.size(); ++i) {
      const auto& m = Driver::kOutSocks[i];
      out_.emplace_back(new OutSock(this, {&m, [](auto){}}));
    }
    for (size_t i = 0; i < Driver::kInSocks.size(); ++i) {
      const auto& m = Driver::kInSocks[i];
      in_.emplace_back(new CustomInSock(this, &m, i));
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

  void Update(RefStack& ref, Event&) noexcept override {
    path_ = ref.GetFullPath();
  }
  void UpdateNode(RefStack&, const std::shared_ptr<Editor>&) noexcept override;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node>(t).Select(this);
  }

  const Path& path() const noexcept { return path_; }

 private:
  std::shared_ptr<std::monostate> life_;

  Path path_;


  std::shared_ptr<Driver> GetDriver(const std::shared_ptr<Context>& ctx) noexcept {
    return ctx->data<Driver>(this, this, std::weak_ptr<Context>(ctx));
  }


  class CustomInSock final : public InSock {
   public:
    CustomInSock(LambdaNode* o, const SockMeta* meta, size_t idx) noexcept:
        InSock(o, {meta, [](auto){}}),
        owner_(o), life_(o->life_), idx_(idx) {
    }

    void Receive(const std::shared_ptr<Context>& ctx, Value&& v) noexcept override {
      if (life_.expired()) return;
      try {
        owner_->GetDriver(ctx)->Handle(idx_, std::move(v));
      } catch (Exception& e) {
        notify::Warn(owner_->path(), owner_,
                     "error while handling input ("+name()+"): "s+e.msg());
      }
    }

    LambdaNode* owner() const noexcept { return owner_; }
    size_t idx() const noexcept { return idx_; }

   private:
    LambdaNode* owner_;

    std::weak_ptr<std::monostate> life_;

    size_t idx_;
  };
};
template <typename Driver>
void LambdaNode<Driver>::UpdateNode(
    RefStack&, const std::shared_ptr<Editor>& ctx) noexcept {
  const auto driver = GetDriver(ctx);
  ImGui::TextUnformatted(driver->title().c_str());

  ImGui::BeginGroup();
  for (size_t i = 0; i < Driver::kInSocks.size(); ++i) {
    const auto& m = Driver::kInSocks[i];
    if (ImNodes::BeginInputSlot(m.name.c_str(), 1)) {
      if (m.type == SockMeta::kPulse) {
        gui::NodeInSock(ctx, in_[i]);
      } else {
        gui::NodeInSock(m.name);
      }
      ImNodes::EndSlot();

      if (m.desc.size() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", m.desc.c_str());
      }
    }
  }
  ImGui::EndGroup();

  ImGui::SameLine();

  ImGui::BeginGroup();
  float w = 0;
  for (const auto& m : Driver::kOutSocks) {
    w = std::max(w, ImGui::CalcTextSize(m.name.c_str()).x);
  }
  const auto left = ImGui::GetCursorPosX();
  for (const auto& m : Driver::kOutSocks) {
    ImGui::SetCursorPosX(left+w-ImGui::CalcTextSize(m.name.c_str()).x);
    if (ImNodes::BeginOutputSlot(m.name.c_str(), 1)) {
      gui::NodeOutSock(m.name);
      ImNodes::EndSlot();
      if (m.desc.size() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", m.desc.c_str());
      }
    }
  }
  ImGui::EndGroup();
}

}  // namespace kingtaker
