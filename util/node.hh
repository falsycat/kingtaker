#pragma once

#include "kingtaker.hh"

#include "iface/node.hh"

#include "util/notify.hh"
#include "util/ptr_selector.hh"
#include "util/value.hh"


namespace kingtaker {

// An implementation of Context that redirects an output of target node to a
// specific socket.
class NodeRedirectContext final : public iface::Node::Context {
 public:
  using Node = iface::Node;

  NodeRedirectContext(const std::weak_ptr<Node::OutSock>& dst,
                  const std::weak_ptr<Node::Context>& ctx,
                  Node*                               target = nullptr) noexcept :
      dst_(dst), ctx_(ctx), target_(target) {
  }

  void Attach(Node* target) noexcept {
    target_ = target;
  }

  void ObserveSend(const Node::OutSock& src, const Value& v) noexcept override {
    if (&src.owner() != target_) return;

    auto dst = dst_.lock();
    auto ctx = ctx_.lock();
    if (dst && ctx) {
      dst->Send(ctx, Value::Tuple { src.name(), Value(v) });
    }
  }

  Node* target() const noexcept { return target_; }

 private:
  std::weak_ptr<Node::OutSock> dst_;
  std::weak_ptr<Node::Context> ctx_;

  Node* target_;
};


// An implemetation of InSock that executes lambda when received something
class NodeLambdaInSock final : public iface::Node::InSock {
 public:
  using Node     = iface::Node;
  using Receiver = std::function<void(const std::shared_ptr<Node::Context>&, Value&&)>;

  NodeLambdaInSock(Node* o, std::string_view n, Receiver&& f) noexcept :
      InSock(o, n), lambda_(std::move(f)) {
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
  using InSock   = Node::InSock;
  using OutSock  = Node::OutSock;
  using Context  = Node::Context;

  // static constexpr char* kTitle = "";

  enum SockFlag : uint8_t {
    kNone        = 0,
    kPulseButton = 1 << 0,
    kFrameHeight = 1 << 1,

    kExecIn   = 1 << 2,
    kErrorOut = 1 << 3,
  };
  using SockFlags = uint8_t;

  struct SockMeta final {
   public:
    SockMeta(const std::string& n, const std::string& d, SockFlags f = kNone) noexcept :
        name(n), desc(d), flags(f) {
    }
    std::string name;
    std::string desc;
    SockFlags   flags;
  };
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
  LambdaNode(const std::shared_ptr<Env>& env) noexcept :
      File(&Driver::kType, env), Node(Node::kNone),
      life_(std::make_shared<std::monostate>()) {
    std::shared_ptr<OutSock> err;
    for (size_t i = 0; i < Driver::kOutSocks.size(); ++i) {
      const auto& m = Driver::kOutSocks[i];
      out_.emplace_back(new OutSock(this, m.name));

      if (m.flags & LambdaNodeDriver::kErrorOut) {
        assert(!err);
        err = out_.back();
      }
    }
    for (size_t i = 0; i < Driver::kInSocks.size(); ++i) {
      const auto& m = Driver::kInSocks[i];

      if (m.flags & LambdaNodeDriver::kExecIn) {
        in_.emplace_back(new ClockInSock(this, m.name, i, err));
      } else {
        in_.emplace_back(new CustomInSock(this, m.name, i));
      }
    }
  }

  static std::unique_ptr<File> Deserialize(
      const msgpack::object&, const std::shared_ptr<Env>& env) noexcept {
    return std::make_unique<LambdaNode>(env);
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_nil();
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<LambdaNode>(env);
  }

  void Update(RefStack& ref, Event&) noexcept override {
    path_ = ref.GetFullPath();
  }
  void Update(RefStack&, const std::shared_ptr<Context>&) noexcept override;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node>(t).Select(this);
  }

  const Path& path() const noexcept { return path_; }

 private:
  std::shared_ptr<std::monostate> life_;

  Path path_;


  std::shared_ptr<Driver> GetDriver(const std::shared_ptr<Context>& ctx) noexcept {
    return ctx->GetOrNew<Driver>(this, this, std::weak_ptr<Context>(ctx));
  }

  // InSock for generic inputs
  class CustomInSock : public InSock {
   public:
    CustomInSock(LambdaNode* o, std::string_view name, size_t idx) noexcept:
        InSock(o, name), owner_(o), life_(o->life_), idx_(idx) {
    }

    void Receive(const std::shared_ptr<Context>& ctx, Value&& v) noexcept override {
      if (life_.expired()) return;
      try {
        owner_->GetDriver(ctx)->Handle(idx_, std::move(v));
      } catch (Exception& e) {
        OnCatch(ctx, e);
      }
    }

    LambdaNode* owner() const noexcept { return owner_; }
    size_t idx() const noexcept { return idx_; }

   protected:
    virtual void OnCatch(const std::shared_ptr<Context>&, Exception& e) noexcept {
      notify::Warn(owner_->path(), owner_,
                   "error while handling input ("+name()+"): "s+e.msg());
    }

   private:
    LambdaNode* owner_;

    std::weak_ptr<std::monostate> life_;

    size_t idx_;
  };

  // InSock for clock input
  class ClockInSock : public CustomInSock {
   public:
    ClockInSock(LambdaNode*      o,
                std::string_view name,
                size_t           idx,
                const std::shared_ptr<OutSock>& err) noexcept :
        CustomInSock(o, name, idx), err_(err) {
    }

    using CustomInSock::owner;
    using CustomInSock::name;

   protected:
    void OnCatch(const std::shared_ptr<Context>& ctx, Exception& e) noexcept {
      notify::Error(owner()->path(), owner(),
                    "error while handling input ("+name()+"): "s+e.msg());
      if (err_) err_->Send(ctx, {});
    }

   private:
    std::shared_ptr<OutSock> err_;
  };
};
template <typename Driver>
void LambdaNode<Driver>::Update(RefStack&, const std::shared_ptr<Context>& ctx) noexcept {
  const auto driver = ctx->GetOrNew<Driver>(this, this, ctx);
  ImGui::TextUnformatted(driver->title().c_str());

  ImGui::BeginGroup();
  for (size_t i = 0; i < Driver::kInSocks.size(); ++i) {
    const auto& m = Driver::kInSocks[i];
    if (ImNodes::BeginInputSlot(m.name.c_str(), 1)) {
      const bool fh = m.flags & LambdaNodeDriver::kFrameHeight;

      if (m.flags & LambdaNodeDriver::kPulseButton) {
        gui::NodeInSock(ctx, in_[i], !fh /* = small */);
      } else {
        if (fh) ImGui::AlignTextToFramePadding();
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
