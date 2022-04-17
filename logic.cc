#include "kingtaker.hh"

#include <optional>

#include <imgui.h>
#include <ImNodes.h>

#include "iface/node.hh"

#include "util/gui.hh"
#include "util/node.hh"
#include "util/notify.hh"
#include "util/ptr_selector.hh"
#include "util/value.hh"

namespace kingtaker {
namespace {

class Passthru final : public File, public iface::Node {
 public:
  static inline TypeInfo kType = TypeInfo::New<Passthru>(
      "Logic/Passthru", "passes all inputs into output directly",
      {typeid(iface::Node)});

  static inline const SockMeta kInMeta = {
    .name = "in", .type = SockMeta::kAny, .trigger = true,
  };
  static inline const SockMeta kOutMeta = {
    .name = "out", .type = SockMeta::kAny,
  };

  Passthru(Env* env) noexcept : File(&kType, env), Node(kNone) {
    out_.emplace_back(new OutSock(this, kOutMeta.gshared()));

    auto task = [out = out_[0]](const auto& ctx, auto&& v) {
      out->Send(ctx, std::move(v));
    };
    in_.emplace_back(new NodeLambdaInSock(this, kInMeta.gshared(), std::move(task)));
  }

  Passthru(Env* env, const msgpack::object&) noexcept : Passthru(env) {
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_nil();
  }
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    return std::make_unique<Passthru>(env);
  }

  void UpdateNode(RefStack&, const std::shared_ptr<Editor>&) noexcept override;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node>(t).Select(this);
  }
};
void Passthru::UpdateNode(RefStack&, const std::shared_ptr<Editor>&) noexcept {
  ImGui::TextUnformatted("PASSTHRU");

  if (ImNodes::BeginInputSlot("in", 1)) {
    gui::NodeSocket();
    ImNodes::EndSlot();
  }

  ImGui::SameLine();
  ImGui::TextUnformatted("->");
  ImGui::SameLine();

  if (ImNodes::BeginOutputSlot("out", 1)) {
    gui::NodeSocket();
    ImNodes::EndSlot();
  }
}


class SetAndGet final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<SetAndGet>;

  static inline TypeInfo kType = TypeInfo::New<Owner>(
      "Logic/SetAndGet", "set any Value, get anytime",
      {typeid(iface::Node)});

  static inline const std::vector<SockMeta> kInSocks = {
    { .name = "clear", .type = SockMeta::kPulse, .trigger = true, },
    { .name = "set",   .type = SockMeta::kAny, },
    { .name = "get",   .type = SockMeta::kPulse, .trigger = true, },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { .name = "out",  .type = SockMeta::kAny,   },
    { .name = "null", .type = SockMeta::kPulse, },
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


class Await final : public File, public iface::Node {
 public:
  static inline TypeInfo kType = TypeInfo::New<Await>(
      "Logic/Await", "passes all inputs into output directly",
      {typeid(iface::Node)});

  static inline const SockMeta kOutMeta = {
    .name = "pulse", .type = SockMeta::kPulse,
  };

  static constexpr size_t kMaxIn = 16;
  static inline const std::vector<SockMeta> kInMeta = ([]() {
    std::vector<SockMeta> meta;
    for (size_t i = 0; i < kMaxIn; ++i) {
      meta.push_back({
            .name = std::to_string(i),
            .type = SockMeta::kPulse,
          });
    }
    return meta;
  })();

  Await(Env* env) noexcept :
      File(&kType, env), Node(kNone), life_(new std::monostate) {
    out_.emplace_back(new OutSock(this, kOutMeta.gshared()));

    in_.resize(kMaxIn);
    for (size_t i = 0; i < kMaxIn; ++i) {
      if (!in_[i]) in_[i] = std::make_shared<CustomInSock>(this, i);
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

  void UpdateNode(RefStack&, const std::shared_ptr<Editor>&) noexcept override;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node>(t).Select(this);
  }

 private:
  std::shared_ptr<std::monostate> life_;


  class ContextData final : public Context::Data {
   public:
    std::array<bool, kMaxIn> recv;
  };

  class CustomInSock final : public InSock {
   public:
    CustomInSock(Await* owner, size_t idx) noexcept :
        InSock(owner, kInMeta[idx].gshared()),
        owner_(owner), life_(owner->life_), idx_(idx) {
      assert(idx_ < kMaxIn);
    }
    void Receive(const std::shared_ptr<Context>& ctx, Value&&) noexcept {
      if (life_.expired()) return;
      auto cdata = ctx->data<ContextData>(owner());

      auto& recv = cdata->recv;
      recv[idx_] = true;

      for (size_t i = 0; i < kMaxIn; ++i) {
        if (ctx->srcOf(owner_->in_[i].get()).size() > 0) {
          if (!recv[i]) return;
        }
      }
      owner_->out_[0]->Send(ctx, {});
      recv.fill(false);
    }

   private:
    Await* owner_;

    std::weak_ptr<std::monostate> life_;

    size_t idx_;
  };
};
void Await::UpdateNode(RefStack&, const std::shared_ptr<Editor>& ctx) noexcept {
  auto cdata = ctx->data<ContextData>(this);

  const auto& recv = cdata->recv;
  ImGui::Text("AWAIT");

  // find last socket that is connected to somewhere
  size_t n = 1;
  for (size_t i = 0; i < kMaxIn; ++i) {
    const size_t idx = kMaxIn-i-1;
    if (ctx->srcOf(in_[idx].get()).size() > 0) {
      n = idx+2;
      break;
    }
  }
  n = std::min(n, kMaxIn);

  // input pins
  ImGui::BeginGroup();
  for (size_t i = 0; i < n; ++i) {
    if (ImNodes::BeginInputSlot(in_[i]->name().c_str(), 1)) {
      gui::NodeSocket();

      const char* msg;
      if (i+1 == n && n < kMaxIn) {
        ImGui::SameLine();
        ImGui::TextUnformatted("?");
        msg = "connect something here to add more inputs";
      } else if (ctx->srcOf(in_[i].get()).size() == 0) {
        ImGui::SameLine();
        ImGui::TextUnformatted("?");
        msg = "no input (treated as pulse is received)";
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
  if (ImNodes::BeginOutputSlot("pulse", 1)) {
    gui::NodeSocket();
    ImNodes::EndSlot();

    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("pulse is emitted when all inputs receive pulse");
    }
  }
}


class Once final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<Once>;

  static inline TypeInfo kType = TypeInfo::New<Owner>(
      "Logic/Once", "emits pulse one time when got anything, does nothing after that",
      {typeid(iface::Node)});

  static inline const std::vector<SockMeta> kInSocks = {
    { .name = "clear", .type = SockMeta::kPulse, .trigger = true, },
    { .name = "in",    .type = SockMeta::kPulse, .trigger = true, },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { .name = "out", .type = SockMeta::kPulse, },
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
      triggered_ = false;
      return;
    case 1:
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
