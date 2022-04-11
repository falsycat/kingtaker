#include "kingtaker.hh"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <fstream>
#include <memory>
#include <optional>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <ImNodes.h>

#include "iface/dir.hh"
#include "iface/memento.hh"
#include "iface/node.hh"

#include "util/format.hh"
#include "util/gui.hh"
#include "util/memento.hh"
#include "util/node.hh"
#include "util/notify.hh"
#include "util/ptr_selector.hh"
#include "util/value.hh"


namespace kingtaker {
namespace {

class Imm final : public File,
    public iface::DirItem,
    public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<Imm>(
      "Value/Imm", "immediate value",
      {typeid(iface::Memento), typeid(iface::DirItem), typeid(iface::Node)});

  Imm(const std::shared_ptr<Env>& env, Value&& v = Value::Integer {0}, ImVec2 size = {0, 0}) noexcept :
      File(&type_, env), DirItem(DirItem::kTree), Node(Node::kNone),
      mem_(std::make_shared<Memento>(UniversalData {this, std::move(v), size})) {
    out_.emplace_back(new OutSock(this, "out"));

    auto receiver = [out = out_[0], mem = mem_](const auto& ctx, auto&&) {
      out->Send(ctx, Value(mem->data().value));
    };
    in_.emplace_back(new NodeLambdaInSock(this, "CLK", std::move(receiver)));
  }

  Imm(const std::shared_ptr<Env>& env, const msgpack::object& obj) :
      Imm(env,
          Value(msgpack::find(obj, "value"s)),
          msgpack::as_if<ImVec2>(msgpack::find(obj, "size"s), {0, 0})) {
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(2);

    const auto& data = mem_->data();

    pk.pack("size");
    pk.pack(std::make_pair(data.size.x, data.size.y));

    pk.pack("value"s);
    data.value.Serialize(pk);
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    const auto& data = mem_->data();
    return std::make_unique<Imm>(env, Value(data.value), data.size);
  }

  void UpdateTree(RefStack&) noexcept override;
  void Update(RefStack&, const std::shared_ptr<Context>&) noexcept override;
  void UpdateTypeChanger(bool mini = false) noexcept;
  void UpdateEditor() noexcept;

  Time lastmod() const noexcept override {
    return lastmod_;
  }
  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::DirItem, iface::Memento, iface::Node>(t).
        Select(this, mem_.get());
  }

 private:
  class UniversalData final {
   public:
    UniversalData(Imm* o, Value&& v, ImVec2 sz) noexcept :
        value(std::move(v)), size(sz), owner_(o) {
    }

    bool operator==(const UniversalData& other) const noexcept {
      return
          value == other.value &&
          size.x == other.size.x && size.y == other.size.y;
    }
    void Restore(const UniversalData& src) noexcept {
      *this = src;
      owner_->OnUpdate();
    }

    Value  value;
    ImVec2 size;

   private:
    Imm* owner_;
  };
  using Memento = SimpleMemento<UniversalData>;
  std::shared_ptr<Memento> mem_;

  // permanentized
  Time lastmod_;


  void OnUpdate() noexcept {
    lastmod_ = Clock::now();
  }
};
void Imm::UpdateTree(RefStack&) noexcept {
  UpdateTypeChanger();
  ImGui::SameLine();
  UpdateEditor();
}
void Imm::Update(RefStack&, const std::shared_ptr<Context>& ctx) noexcept {
  ImGui::TextUnformatted("IMM:");
  ImGui::SameLine();
  UpdateTypeChanger(true);

  if (ImNodes::BeginInputSlot("CLK", 1)) {
    ImGui::AlignTextToFramePadding();
    gui::NodeSocket();
    ImGui::SameLine();
    if (ImGui::Button("CLK")) {
      Queue::sub().Push([clk = in_[0], ctx]() { clk->Receive(ctx, {}); });
    }
    ImNodes::EndSlot();
  }

  ImGui::SameLine();
  ImGui::BeginGroup();
  UpdateEditor();
  ImGui::EndGroup();
  ImGui::SameLine();

  if (ImNodes::BeginOutputSlot("out", 1)) {
    ImGui::AlignTextToFramePadding();
    gui::NodeSocket();
    ImNodes::EndSlot();
  }
}
void Imm::UpdateTypeChanger(bool mini) noexcept {
  auto& v = mem_->data().value;

  const char* type =
      v.isInteger()? "Int":
      v.isScalar()?  "Sca":
      v.isBoolean()? "Boo":
      v.isString()?  "Str": "XXX";
  mini? ImGui::SmallButton(type): ImGui::Button(type);

  gui::NodeCanvasResetZoom();
  if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
    if (ImGui::MenuItem("integer", nullptr, v.isInteger())) {
      v = Value::Integer {0};
      OnUpdate();
    }
    if (ImGui::MenuItem("scalar", nullptr, v.isScalar())) {
      v = Value::Scalar {0};
      OnUpdate();
    }
    if (ImGui::MenuItem("boolean", nullptr, v.isBoolean())) {
      v = Value::Boolean {false};
      OnUpdate();
    }
    if (ImGui::MenuItem("string", nullptr, v.isString())) {
      v = ""s;
      OnUpdate();
    }
    ImGui::EndPopup();
  }
  gui::NodeCanvasSetZoom();
}
void Imm::UpdateEditor() noexcept {
  const auto em = ImGui::GetFontSize();
  const auto fh = ImGui::GetFrameHeight();

  auto& data = mem_->data();
  auto& v    = data.value;

  ImGui::SameLine();
  if (v.isInteger()) {
    gui::ResizeGroup _("##resizer", &data.size, {4, fh/em}, {12, fh/em}, em);
    ImGui::SetNextItemWidth(data.size.x*em);
    if (ImGui::DragScalar("##editor", ImGuiDataType_S64, &v.integer())) {
      OnUpdate();
    }

  } else if (v.isScalar()) {
    gui::ResizeGroup _("##resizer", &data.size, {4, fh/em}, {12, fh/em}, em);
    ImGui::SetNextItemWidth(data.size.x*em);
    if (ImGui::DragScalar("##editor", ImGuiDataType_Double, &v.scalar())) {
      OnUpdate();
    }

  } else if (v.isBoolean()) {
    if (ImGui::Checkbox("##editor", &v.boolean())) {
      OnUpdate();
    }

  } else if (v.isString()) {
    gui::ResizeGroup _("##resizer", &data.size, {4, fh/em}, {24, 24}, em);
    if (ImGui::InputTextMultiline("##editor", &v.stringUniq(), data.size*em)) {
      OnUpdate();
    }

  } else {
    ImGui::TextUnformatted("UNKNOWN TYPE X(");
  }

  if (!ImGui::IsAnyItemActive() && data != mem_->commitData()) {
    mem_->Commit();
  }
}


class Tuple1 final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<Tuple1>;

  static inline TypeInfo kType = TypeInfo::New<Owner>(
      "Value/Tuple1", "creates 1 element tuple",
      {typeid(iface::Node)});

  static inline const std::vector<SockMeta> kInSocks = {
    { "item", "", },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { "tuple", "", },
  };

  Tuple1(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  std::string title() const noexcept {
    return "TUPLE1";
  }

  void Handle(size_t idx, Value&& v) {
    switch (idx) {
    case 0:
      owner_->out(0)->Send(ctx_.lock(), Value::Tuple { std::move(v) });
      return;
    }
    assert(false);
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> ctx_;
};


class TuplePush final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<TuplePush>;

  static inline TypeInfo kType = TypeInfo::New<Owner>(
      "Value/TuplePush", "append 1 element to tuple",
      {typeid(iface::Node)});

  static inline const std::vector<SockMeta> kInSocks = {
    { "tuple", "", },
    { "item",  "", },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { "tuple", "", },
  };

  TuplePush(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  std::string title() const noexcept {
    return "TUPLE PUSH";
  }

  void Handle(size_t idx, Value&& v) {
    switch (idx) {
    case 0:
      tuple_ = v.tupleUniqPtr();
      ExecIf();
      return;
    case 1:
      item_ = std::move(v);
      ExecIf();
      return;
    }
    assert(false);
  }
  void ExecIf() noexcept {
    if (!tuple_ || !item_) return;
    tuple_->push_back(std::move(*item_));
    owner_->out(0)->Send(ctx_.lock(), tuple_);

    tuple_ = nullptr;
    item_  = std::nullopt;
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> ctx_;

  std::shared_ptr<Value::Tuple> tuple_;

  std::optional<Value> item_;
};


class TuplePop final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<TuplePop>;

  static inline TypeInfo kType = TypeInfo::New<Owner>(
      "Value/TuplePop", "pop the last element from tuple",
      {typeid(iface::Node)});

  static inline const std::vector<SockMeta> kInSocks = {
    { "tuple", "", kExecIn },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { "item",  "", },
    { "tuple", "", },
  };

  TuplePop(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  std::string title() const noexcept {
    return "TUPLE POP";
  }

  void Handle(size_t idx, Value&& v) {
    assert(idx == 0);
    (void) idx;

    auto  ctx = ctx_.lock();
    auto& tup = v.tupleUniq();
    if (tup.size() == 0) throw Exception("empty tuple");

    owner_->out(0)->Send(ctx, Value(tup.back()));
    tup.pop_back();
    owner_->out(1)->Send(ctx, std::move(v));
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> ctx_;
};

} }  // namespace kingtaker
