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


class Satisfy final : public File, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<Satisfy>(
      "Logic/Satisfy", "emits pulse when all inputs are satisfied",
      {typeid(iface::Node)});

  static constexpr size_t kMaxInput = 16;

  Satisfy(const std::shared_ptr<Env>& env, size_t n = 0) noexcept :
      File(&type_, env), Node(kNone) {
    data_ = std::make_shared<UniversalData>();

    out_.emplace_back(new OutSock(this, "CLK"));

    auto task = [self = this](const auto& ctx, auto&&) {
      auto& conds = ctx->template GetOrNew<ContextData>(self)->conds;
      conds.clear();
    };
    clr_ = std::make_shared<LambdaInSock>(this, "CLR", std::move(task));;

    Rebuild(n);
  }

  static std::unique_ptr<File> Deserialize(
      const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    try {
      const auto n = msgpack::as_if<size_t>(obj, 0);
      if (n > kMaxInput) {
        throw DeserializeException("input count overflow");
      }
      return std::make_unique<Satisfy>(env, n);
    } catch (msgpack::type_error&) {
      throw DeserializeException("broken Logic/Satisfy");
    }
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack(data_->n);
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<Satisfy>(env, data_->n);
  }

  void Update(RefStack&, const std::shared_ptr<Context>& ctx) noexcept override;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node>(t).Select(this);
  }

 private:
  struct UniversalData {
   public:
    size_t n = 0;
  };
  std::shared_ptr<UniversalData> data_;

  std::shared_ptr<InSock> clr_;
  std::vector<std::shared_ptr<InSock>> in_sats_;


  // recreates input sockets
  void Rebuild(size_t n) noexcept {
    in_sats_.resize(n);
    for (size_t i = data_->n; i < n; ++i) {
      in_sats_[i] = std::make_shared<SatisfySock>(this, i);
    }
    data_->n = n;

    in_.clear();
    in_.push_back(clr_);
    for (auto& sock : in_sats_) in_.push_back(sock);
  }


  class ContextData final : public Context::Data {
   public:
    std::vector<bool> conds;
  };
  class SatisfySock final : public InSock {
   public:
    SatisfySock(Satisfy* o, size_t idx) noexcept :
        InSock(o, std::string(1, static_cast<char>('A'+idx))),
        data_(o->data_), out_(o->out_[0]), idx_(idx) {
    }
    void Receive(const std::shared_ptr<Context>& ctx, Value&&) noexcept override {
      auto data = data_.lock();
      auto out  = out_.lock();
      if (!data || !out) return;

      auto& conds = ctx->GetOrNew<ContextData>(&owner())->conds;
      if (conds.size() != data->n) {
        const size_t pn = conds.size();
        conds.resize(data->n);
        for (size_t i = pn; i < conds.size(); ++i) {
          conds[i] = false;
        }
      }
      if (idx_ >= conds.size()) return;

      conds[idx_] = true;
      for (const auto b : conds) if (!b) return;
      out->Send(ctx, {});
      for (auto b : conds) b = false;
    }
   private:
    std::weak_ptr<UniversalData> data_;

    std::weak_ptr<OutSock> out_;

    size_t idx_;
  };
};
void Satisfy::Update(RefStack&, const std::shared_ptr<Context>& ctx) noexcept {
  ImGui::TextUnformatted("SATISFY");

  const auto em = ImGui::GetFontSize();

  auto  ctxdata = ctx->GetOrNew<ContextData>(this);
  auto& conds   = ctxdata->conds;

  if (ImNodes::BeginInputSlot("CLR", 1)) {
    ImGui::AlignTextToFramePadding();
    gui::NodeSocket();
    ImGui::SameLine();
    if (ImGui::Button("CLR")) {
      Queue::sub().Push([clr = clr_, ctx]() { clr->Receive(ctx, {}); });
    }
    ImNodes::EndSlot();
  }

  ImGui::SameLine();
  ImGui::SetNextItemWidth(2*em);
  int n = static_cast<int>(data_->n);
  if (ImGui::DragInt("##InputCount", &n, 1, 0, kMaxInput)) {
    Queue::main().Push([this, n]() { Rebuild(static_cast<size_t>(n)); });
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("number of inputs\n"
                      "pulse is emitted when all input received something");
  }

  ImGui::SameLine();
  if (ImNodes::BeginOutputSlot("CLK", 1)) {
    if (ImGui::Button("CLK")) {
      out_[0]->Send(ctx, {});
    }
    ImGui::SameLine();
    gui::NodeSocket();
    ImNodes::EndSlot();
  }

  for (size_t i = 0; i < data_->n; ++i) {
    const auto& name = in_sats_[i]->name();
    if (ImNodes::BeginInputSlot(name.c_str(), 1)) {
      gui::NodeSocket();
      ImGui::SameLine();

      if (i < conds.size() && conds[i]) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
        ImGui::TextUnformatted(name.c_str());
        ImGui::PopStyleColor();
      } else {
        ImGui::TextUnformatted(name.c_str());
      }
      ImNodes::EndSlot();
    }
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

}}  // namespace kingtaker
