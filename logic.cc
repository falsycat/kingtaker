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
  static inline TypeInfo type_ = TypeInfo::New<Passthru>(
      "Logic/Passthru", "passes all inputs into output directly",
      {typeid(iface::Node)});

  Passthru(const std::shared_ptr<Env>& env) noexcept :
      File(&type_, env), Node(kNone) {
    out_.emplace_back(new OutSock(this, "out"));

    std::weak_ptr<OutSock> wout = out_[0];
    auto task = [wout](const auto& ctx, auto&& v) {
      auto out = wout.lock();
      if (!out) return;
      out->Send(ctx, std::move(v));
    };
    in_.emplace_back(new LambdaInSock(this, "in", std::move(task)));
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object&, const std::shared_ptr<Env>& env) {
    return std::make_unique<Passthru>(env);
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_nil();
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<Passthru>(env);
  }

  void Update(RefStack&, const std::shared_ptr<Context>&) noexcept override;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node>(t).Select(this);
  }
};
void Passthru::Update(RefStack&, const std::shared_ptr<Context>&) noexcept {
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

  static inline TypeInfo type_ = TypeInfo::New<Owner>(
      "Logic/SetAndGet", "set any Value, get anytime",
      {typeid(iface::Node)});

  static inline const std::vector<SockMeta> kInSocks = {
    { "CLR", "", kPulseButton },
    { "set", "", },
    { "get", "", kPulseButton },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { "out",  "", },
    { "null", "", },
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


class Await final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<Await>;

  static inline TypeInfo type_ = TypeInfo::New<Owner>(
      "Logic/Await", "emits pulse when got as many values as number of connections",
      {typeid(iface::Node)});

  static inline const std::vector<SockMeta> kInSocks = {
    { "CLR", "", kPulseButton },
    { "in", "", },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { "out", "", },
  };

  Await() = delete;
  Await(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  std::string title() const noexcept {
    return "AWAIT ("+std::to_string(count_)+"/"+std::to_string(expect())+")";
  }

  void Handle(size_t idx, Value&&) {
    switch (idx) {
    case 0:
      count_ = 0;
      return;
    case 1:
      if (++count_ >= expect()) {
        owner_->out()[0]->Send(ctx_.lock(), {});
        count_ = 0;
      }
      return;
    }
    assert(false);
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> ctx_;

  size_t count_ = 0;

  size_t expect() const noexcept {
    return owner_->in(1)->src().size();
  }
};


class Once final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<Once>;

  static inline TypeInfo type_ = TypeInfo::New<Owner>(
      "Logic/Once", "emits pulse one time when got anything, does nothing after that",
      {typeid(iface::Node)});

  static inline const std::vector<SockMeta> kInSocks = {
    { "CLR", "", kPulseButton },
    { "in", "", },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { "out", "", },
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
