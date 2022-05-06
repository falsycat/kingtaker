#include "kingtaker.hh"

#include <optional>

#include <imgui.h>
#include <ImNodes.h>

#include "iface/node.hh"

#include "util/gui.hh"
#include "util/node.hh"
#include "util/ptr_selector.hh"
#include "util/value.hh"

namespace kingtaker {
namespace {

class Passthru final : public File, public iface::Node {
 public:
  static inline TypeInfo kType = TypeInfo::New<Passthru>(
      "Logic/Passthru", "passes all inputs into output directly",
      {typeid(iface::Node)});

  Passthru(Env* env) noexcept :
      File(&kType, env), Node(kNone),
      sock_out_(this, "out"),
      sock_in_(this, "in", [this](auto& ctx, auto&& v) { sock_out_.Send(ctx, std::move(v)); }) {
    out_ = {&sock_out_};
    in_  = {&sock_in_};
  }

  Passthru(Env* env, const msgpack::object&) noexcept : Passthru(env) {
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_nil();
  }
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    return std::make_unique<Passthru>(env);
  }

  void UpdateNode(const std::shared_ptr<Editor>&) noexcept override;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node>(t).Select(this);
  }

 private:
  OutSock          sock_out_;
  NodeLambdaInSock sock_in_;
};
void Passthru::UpdateNode(const std::shared_ptr<Editor>&) noexcept {
  ImGui::TextUnformatted("PASSTHRU");

  if (ImNodes::BeginInputSlot("in", 1)) {
    gui::NodeSockPoint();
    ImNodes::EndSlot();
  }

  ImGui::SameLine();
  ImGui::TextUnformatted("->");
  ImGui::SameLine();

  if (ImNodes::BeginOutputSlot("out", 1)) {
    gui::NodeSockPoint();
    ImNodes::EndSlot();
  }
}


class Await final : public File, public iface::Node {
 public:
  static inline TypeInfo kType = TypeInfo::New<Await>(
      "Logic/Await", "passes all inputs into output directly",
      {typeid(iface::Node)});

  static constexpr size_t kMaxIn = 16;

  Await(Env* env) noexcept :
      File(&kType, env), Node(kNone), sock_out_(this, "out") {
    out_.push_back(&sock_out_);

    in_.resize(kMaxIn);
    for (size_t i = 0; i < kMaxIn; ++i) {
      sock_in_[i].emplace(this, i);
      in_[i] = &*sock_in_[i];
    }
  }

  Await(Env* env, const msgpack::object&) noexcept : Await(env) {
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_nil();
  }
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    return std::make_unique<Await>(env);
  }

  void UpdateNode(const std::shared_ptr<Editor>&) noexcept override;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node>(t).Select(this);
  }

 private:
  class ContextData final : public Context::Data {
   public:
    std::array<bool, kMaxIn> recv;
  };

  class CustomInSock final : public InSock {
   public:
    CustomInSock(Await* owner, size_t idx) noexcept :
        InSock(owner, std::string(1, static_cast<char>('A'+idx))),
        owner_(owner), idx_(idx) {
      assert(idx_ < kMaxIn);
    }
    void Receive(const std::shared_ptr<Context>& ctx, Value&&) noexcept {
      auto cdata = ctx->data<ContextData>(owner());

      auto& recv = cdata->recv;
      recv[idx_] = true;

      for (size_t i = 0; i < kMaxIn; ++i) {
        if (ctx->srcOf(owner_->in_[i]).size() > 0) {
          if (!recv[i]) return;
        }
      }
      owner_->out_[0]->Send(ctx, {});
      recv.fill(false);
    }

   private:
    Await* owner_;

    size_t idx_;
  };


  OutSock sock_out_;

  std::array<std::optional<CustomInSock>, kMaxIn> sock_in_;
};
void Await::UpdateNode(const std::shared_ptr<Editor>& ctx) noexcept {
  auto cdata = ctx->data<ContextData>(this);

  const auto& recv = cdata->recv;
  ImGui::Text("AWAIT");

  // find last socket that is connected to somewhere
  size_t n = 1;
  for (size_t i = 0; i < kMaxIn; ++i) {
    const size_t idx = kMaxIn-i-1;
    if (ctx->srcOf(in_[idx]).size() > 0) {
      n = idx+2;
      break;
    }
  }
  n = std::min(n, kMaxIn);

  // input pins
  ImGui::BeginGroup();
  for (size_t i = 0; i < n; ++i) {
    if (ImNodes::BeginInputSlot(in_[i]->name().c_str(), 1)) {
      gui::NodeSockPoint();

      const char* msg;
      if (i+1 == n && n < kMaxIn) {
        ImGui::SameLine();
        ImGui::TextUnformatted("?");
        msg = "connect something here to add more inputs";
      } else if (ctx->srcOf(in_[i]).size() == 0) {
        ImGui::SameLine();
        ImGui::TextUnformatted("?");
        msg = "no input (treated as pulse received)";
      } else {
        if (recv[i]) {  // pulse received
          ImGui::SameLine();
          ImGui::TextUnformatted("Z");
          msg = "pulse received";
        } else {
          ImGui::SameLine();
          ImGui::TextUnformatted("/");
          msg = "awaiting pulse";
        }
      }
      ImNodes::EndSlot();

      if (msg && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", msg);
    }
  }
  ImGui::EndGroup();

  // output pin
  ImGui::SameLine();
  if (ImNodes::BeginOutputSlot("out", 1)) {
    gui::NodeSockPoint();
    ImNodes::EndSlot();

    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("pulse is emitted when all inputs receive something");
    }
  }
}


class SetAndGet final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<SetAndGet>;

  static inline TypeInfo kType = TypeInfo::New<Owner>(
      "Logic/SetAndGet", "set any Value, get anytime",
      {typeid(iface::Node)});

  static inline const std::vector<SockMeta> kInSocks = {
    { "clear", "" },
    { "set", "" },
    { "get", "" },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { "out", "" },
    { "null", "" },
  };

  SetAndGet() = delete;
  SetAndGet(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  std::string title() const noexcept {
    if (value_) {
      return "SETnGET*";
    } else {
      return "SETnGET";
    }
  }

  void Handle(size_t idx, Value&& v) {
    switch (idx) {
    case 0:
      value_ = std::nullopt;
      return;
    case 1:
      value_ = std::move(v);
      return;
    case 2:
      if (value_) {
        owner_->out()[0]->Send(ctx_.lock(), Value(*value_));
      } else {
        owner_->out()[1]->Send(ctx_.lock(), {});
      }
      return;
    }
    assert(false);
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> ctx_;

  std::optional<Value> value_;
};


class Once final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<Once>;

  static inline TypeInfo kType = TypeInfo::New<Owner>(
      "Logic/Once", "emits pulse one time when got anything, does nothing after that",
      {typeid(iface::Node)});

  static inline const std::vector<SockMeta> kInSocks = {
    { "in", "" },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { "out", "" },
  };

  Once() = delete;
  Once(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  std::string title() const noexcept {
    return triggered_? "ONCE*": "ONCE";
  }

  void Handle(size_t idx, Value&&) {
    switch (idx) {
    case 0:
      if (!triggered_) {
        owner_->out()[0]->Send(ctx_.lock(), {});
      }
      triggered_ = true;
      return;
    }
    assert(false);
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> ctx_;

  bool triggered_ = false;
};

}}  // namespace kingtaker
