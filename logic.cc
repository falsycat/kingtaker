#include "kingtaker.hh"

#include <imgui.h>
#include <ImNodes.h>

#include "iface/node.hh"

#include "util/gui.hh"
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


class Satisfaction : public File, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<Satisfaction>(
      "Logic/Satisfaction", "emits pulse when all inputs are satisfied",
      {typeid(iface::Node)});

  static constexpr size_t kMaxInput = 16;

  Satisfaction(const std::shared_ptr<Env>& env, size_t n = 0) noexcept :
      File(&type_, env), Node(kNone) {
    data_ = std::make_shared<UniversalData>();

    out_.emplace_back(new OutSock(this, "out"));

    auto task = [self = this](const auto& ctx, auto&&) {
      auto& conds = ctx->template GetOrNew<ContextData>(self)->conds;
      for (auto b : conds) b = false;
    };
    clear_ = std::make_shared<LambdaInSock>(this, "clear", std::move(task));;

    Rebuild(n);
  }

  static std::unique_ptr<File> Deserialize(
      const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    return std::make_unique<Satisfaction>(env, msgpack::as_if<size_t>(obj, 0));
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack(data_->n);
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<Satisfaction>(env, data_->n);
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

  std::shared_ptr<InSock> clear_;


  void Rebuild(size_t n) noexcept {
    in_.resize(n);
    for (size_t i = data_->n; i < n; ++i) {
      in_[i] = std::make_shared<SatisfySock>(this, i);
    }
    if (in_.size()) in_.push_back(clear_);
    data_->n = n;
  }


  class ContextData final : public Context::Data {
   public:
    std::vector<bool> conds;
  };
  class SatisfySock final : public InSock {
   public:
    SatisfySock(Satisfaction* o, size_t idx) noexcept :
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
      out->Send(ctx, Value::Pulse());
      for (auto b : conds) b = false;
    }
   private:
    std::weak_ptr<UniversalData> data_;

    std::weak_ptr<OutSock> out_;

    size_t idx_;
  };
};
void Satisfaction::Update(RefStack&, const std::shared_ptr<Context>& ctx) noexcept {
  ImGui::TextUnformatted("PULSE");

  const auto em = ImGui::GetFontSize();

  auto  ctxdata = ctx->GetOrNew<ContextData>(this);
  auto& conds   = ctxdata->conds;

  ImGui::BeginGroup();
  {
    int n = static_cast<int>(data_->n);
    ImGui::SetNextItemWidth(2*em);
    if (ImGui::DragInt("##InputCount", &n, 1, 0, kMaxInput)) {
      Queue::main().Push([this, n]() { Rebuild(static_cast<size_t>(n)); });
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("number of inputs\n"
                        "pulse is emitted when all input received something");
    }

    ImGui::SameLine();
    if (ImGui::Button("X")) {
      auto task = [ctxdata]() mutable {
        ctxdata->conds.clear();
        ctxdata = nullptr;
      };
      Queue::main().Push(std::move(task));
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("clears state");
    }

    ImGui::SameLine();
    if (ImGui::Button("Z")) {
      out_[0]->Send(ctx, Value::Pulse());
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("generates pulse manually");
    }

    for (size_t i = 0; i < data_->n; ++i) {
      const auto& name = in_[i]->name();
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
    if (in_.size()) {
      if (ImNodes::BeginInputSlot("clear", 1)) {
        gui::NodeSocket();
        ImGui::SameLine();
        ImGui::TextDisabled("clear");
        ImNodes::EndSlot();
      }
    }
  }
  ImGui::EndGroup();

  ImGui::SameLine();
  if (ImNodes::BeginOutputSlot("out", 1)) {
    ImGui::TextUnformatted("out");
    ImGui::SameLine();
    gui::NodeSocket();
    ImNodes::EndSlot();
  }
}

}}  // namespace kingtaker
