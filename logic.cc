#include "kingtaker.hh"

#include <optional>

#include <imgui.h>
#include <ImNodes.h>

#include "iface/node.hh"

#include "util/gui.hh"
#include "util/notify.hh"
#include "util/ptr_selector.hh"
#include "util/value.hh"

namespace kingtaker {
namespace {

class Equal final : public File, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<Equal>(
      "Logic/Equal", "emits a pulse if all of AND_X is equal to one of OR_X",
      {typeid(iface::Node)});

  static constexpr size_t kMaxInput = 16;

  Equal(const std::shared_ptr<Env>& env, size_t and_n = 1, size_t or_n = 1) noexcept :
      File(&type_, env), Node(kNone) {
    assert(and_n >= 1 && or_n >= 1);

    data_ = std::make_shared<UniversalData>();

    out_and_ = std::make_shared<OutSock>(this, "AND");
    out_or_  = std::make_shared<OutSock>(this, "OR");
    out_     = {out_and_, out_or_};

    std::weak_ptr<UniversalData> wd = data_;
    std::weak_ptr<OutSock>       wa = out_and_;
    std::weak_ptr<OutSock>       wo = out_or_;
    auto clk_task = [self = this, wd, wa, wo](const auto& ctx, auto&&) {
      Proc(self, ctx, wd, wa, wo);
    };
    clk_ = std::make_shared<LambdaInSock>(this, "CLK", std::move(clk_task));

    auto clr_task = [self = this](const auto& ctx, auto&&) {
      auto ctxdata = ctx->template GetOrNew<ContextData>(self);
      ctxdata->and_.clear();
      ctxdata->or_ .clear();
    };
    clr_ = std::make_shared<LambdaInSock>(this, "CLR", std::move(clr_task));
    Rebuild(and_n, or_n);
  }

  static std::unique_ptr<File> Deserialize(
      const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    try {
      const auto a = msgpack::find(obj, "and"s).as<size_t>();
      const auto o = msgpack::find(obj, "or"s).as<size_t>();
      if (a == 0 || a > kMaxInput || o == 0 || o > kMaxInput) {
        throw DeserializeException("Logic/Equal socket count overflow");
      }
      return std::make_unique<Equal>(env, a, o);
    } catch (msgpack::type_error&) {
      throw DeserializeException("broken Logic/Equal");
    }
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(2);

    pk.pack("and");
    pk.pack(data_->and_n);

    pk.pack("or");
    pk.pack(data_->or_n);
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<Equal>(env, data_->and_n, data_->or_n);
  }

  void Update(RefStack&, const std::shared_ptr<Context>&) noexcept override;
  static bool UpdateSocks(const char*, int*, std::span<std::shared_ptr<InSock>>) noexcept;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node>(t).Select(this);
  }

 private:
  struct UniversalData final {
   public:
    File::Path path;

    size_t and_n;
    size_t or_n;
  };
  class ContextData final : public Context::Data {
   public:
    std::vector<std::optional<Value>> and_;
    std::vector<std::optional<Value>> or_;
  };


  std::shared_ptr<UniversalData> data_;

  std::vector<std::shared_ptr<InSock>> in_and_;
  std::vector<std::shared_ptr<InSock>> in_or_;

  std::shared_ptr<OutSock> out_and_;
  std::shared_ptr<OutSock> out_or_;

  std::shared_ptr<InSock> clk_;
  std::shared_ptr<InSock> clr_;


  // handles clk input (called from InSock::Receive())
  static void Proc(Equal*                              self,
                   const std::shared_ptr<Context>&     ctx,
                   const std::weak_ptr<UniversalData>& wd,
                   const std::weak_ptr<OutSock>&       wa,
                   const std::weak_ptr<OutSock>&       wo) noexcept {
    auto data    = wd.lock();
    auto out_and = wa.lock();
    auto out_or  = wo.lock();
    if (!data || !out_and || !out_or) return;

    auto ctxdata = ctx->template GetOrNew<ContextData>(self);
    bool satisfy = (ctxdata->and_.size() == data->and_n &&
                    ctxdata->or_ .size() == data->or_n);
    for (size_t i = 0; satisfy && i < ctxdata->and_.size(); ++i) {
      if (!ctxdata->and_[i]) satisfy = false;
    }
    for (size_t i = 0; satisfy && i < ctxdata->or_.size(); ++i) {
      if (!ctxdata->or_[i]) satisfy = false;
    }
    if (!satisfy) {
      notify::Warn(data->path, self,
                   "all inputs are not satisfied yet, evaluation aborted");
      return;
    }

    bool a = true;
    bool o = false;
    for (auto& av : ctxdata->and_) {
      bool match = false;
      for (const auto& ov : ctxdata->or_) {
        if (*av == *ov) match = true;
      }
      if (match) {
        o = true;
      } else {
        a = false;
      }
    }
    ctxdata->and_.clear();
    ctxdata->or_ .clear();
    if (a) out_and->Send(ctx, Value::Pulse());
    if (o) out_or ->Send(ctx, Value::Pulse());
  }

  // recreates input sockets
  void Rebuild(size_t a, size_t o) noexcept {
    // creates AND sockets
    in_and_.resize(a);
    for (size_t i = data_->and_n; i < a; ++i) {
      in_and_[i] = std::make_shared<AndInSock>(this, i);
    }
    data_->and_n = a;

    // creates OR sockets
    in_or_.resize(o);
    for (size_t i = data_->or_n; i < o; ++i) {
      in_or_[i] = std::make_shared<OrInSock>(this, i);
    }
    data_->or_n  = o;

    // sync input socket list
    in_.clear();
    in_.push_back(clk_);
    in_.push_back(clr_);
    for (auto& sock : in_and_) in_.push_back(sock);
    for (auto& sock : in_or_ ) in_.push_back(sock);
  }


  // common implementation of AND and OR
  class AbstractInSock : public InSock {
   public:
    AbstractInSock(Equal* o, const std::string& prefix, size_t idx) noexcept :
        InSock(o, prefix+std::string(1, static_cast<char>('A'+idx))),
        idx_(idx) {
    }
    void Receive(const std::shared_ptr<Context>& ctx, Value&& v) noexcept override {
      auto  ctxdata = ctx->GetOrNew<ContextData>(&owner());
      auto& list    = GetList(*ctxdata);

      if (list.size() <= idx_) list.resize(idx_+1);
      list[idx_] = std::move(v);
    }

   protected:
    virtual std::vector<std::optional<Value>>& GetList(ContextData&) const noexcept = 0;

   private:
    size_t idx_;
  };
  class AndInSock final : public AbstractInSock {
   public:
    AndInSock(Equal* o, size_t idx) noexcept : AbstractInSock(o, "AND_", idx) {
    }
    std::vector<std::optional<Value>>& GetList(ContextData& data) const noexcept override {
      return data.and_;
    }
  };
  class OrInSock final : public AbstractInSock {
   public:
    OrInSock(Equal* o, size_t idx) noexcept : AbstractInSock(o, "OR_", idx) {
    }
    std::vector<std::optional<Value>>& GetList(ContextData& data) const noexcept override {
      return data.or_;
    }
  };
};
void Equal::Update(RefStack&, const std::shared_ptr<Context>& ctx) noexcept {
  const auto em = ImGui::GetFontSize();

  ImGui::TextUnformatted("EQUAL");

  // input sockets
  ImGui::BeginGroup();
  {
    if (ImNodes::BeginInputSlot("CLK", 1)) {
      gui::NodeSocket();
      ImGui::SameLine();
      if (ImGui::SmallButton("CLK")) {
        Queue::sub().Push([clk = clk_, ctx]() { clk->Receive(ctx, Value::Pulse()); });
      }
      ImNodes::EndSlot();
    }
    if (ImNodes::BeginInputSlot("CLR", 1)) {
      gui::NodeSocket();
      ImGui::SameLine();
      if (ImGui::SmallButton("CLR")) {
        Queue::sub().Push([clr = clr_, ctx]() { clr->Receive(ctx, Value::Pulse()); });
      }
      ImNodes::EndSlot();
    }

    int and_n = static_cast<int>(data_->and_n);
    int or_n  = static_cast<int>(data_->or_n);

    const bool mod =
      UpdateSocks("##AndCount", &and_n, in_and_) | UpdateSocks("##OrCount", &or_n,  in_or_);

    if (mod) {
      auto task = [this, and_n, or_n]() {
        Rebuild(static_cast<size_t>(and_n), static_cast<size_t>(or_n));
      };
      Queue::main().Push(std::move(task));
    }
  }
  ImGui::EndGroup();

  ImGui::SameLine();
  ImGui::Dummy({1*em, 1});
  ImGui::SameLine();

  // output sockets
  ImGui::BeginGroup();
  {
    if (ImNodes::BeginOutputSlot("AND", 1)) {
      if (ImGui::SmallButton("AND")) {
        out_and_->Send(ctx, Value::Pulse());
      }
      ImGui::SameLine();
      gui::NodeSocket();
      ImNodes::EndSlot();
    }
    if (ImNodes::BeginOutputSlot("OR", 1)) {
      if (ImGui::SmallButton(" OR")) {
        out_or_->Send(ctx, Value::Pulse());
      }
      ImGui::SameLine();
      gui::NodeSocket();
      ImNodes::EndSlot();
    }
  }
  ImGui::EndGroup();
}
bool Equal::UpdateSocks(
    const char* id, int* n, std::span<std::shared_ptr<InSock>> socks) noexcept {
  const auto left = ImGui::GetCursorPosX();
  const auto em   = ImGui::GetFontSize();

  ImGui::BeginGroup();
  for (auto& sock : socks) {
    if (ImNodes::BeginInputSlot(sock->name().c_str(), 1)) {
      gui::NodeSocket();
      ImGui::SameLine();
      ImGui::TextUnformatted(sock->name().c_str());
      ImNodes::EndSlot();
    }
  }
  ImGui::EndGroup();

  ImGui::SameLine();
  ImGui::SetNextItemWidth(2*em);
  ImGui::SetCursorPosX(left + 5*em);
  return ImGui::DragInt(id, n, .2f, 1, kMaxInput);
}


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

    out_.emplace_back(new OutSock(this, "out"));

    auto task = [self = this](const auto& ctx, auto&&) {
      auto& conds = ctx->template GetOrNew<ContextData>(self)->conds;
      conds.clear();
    };
    clr_ = std::make_shared<LambdaInSock>(this, "CLR", std::move(task));;

    Rebuild(n);
  }

  static std::unique_ptr<File> Deserialize(
      const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    return std::make_unique<Satisfy>(env, msgpack::as_if<size_t>(obj, 0));
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
    if (n) in_.push_back(clr_);
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
      out->Send(ctx, Value::Pulse());
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

  ImGui::BeginGroup();
  {
    if (in_.size()) {
      if (ImNodes::BeginInputSlot("CLR", 1)) {
        gui::NodeSocket();
        ImGui::SameLine();
        if (ImGui::SmallButton("CLR")) {
          Queue::sub().Push([clr = clr_, ctx]() { clr->Receive(ctx, Value::Pulse()); });
        }
        ImNodes::EndSlot();
      }
    }
    ImGui::BeginGroup();
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
    ImGui::EndGroup();

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
  }
  ImGui::EndGroup();

  ImGui::SameLine();
  if (ImNodes::BeginOutputSlot("out", 1)) {
    if (ImGui::SmallButton("out")) {
      out_[0]->Send(ctx, Value::Pulse());
    }
    ImGui::SameLine();
    gui::NodeSocket();
    ImNodes::EndSlot();
  }
}

}}  // namespace kingtaker
