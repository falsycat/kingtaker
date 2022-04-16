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

  static inline const SockMeta kIn = {
    .name = "clk", .type = SockMeta::kPulse, .trigger = true, .def = Value::Pulse{},
  };
  static inline const SockMeta kOut = {
    .name = "out", .type = SockMeta::kAny,
  };

  Imm(Env* env, Value&& v = Value::Integer {0}, ImVec2 size = {0, 0}) noexcept :
      File(&type_, env), DirItem(DirItem::kTree), Node(Node::kNone),
      mem_(std::make_shared<Memento>(UniversalData {this, std::move(v), size})) {
    out_.emplace_back(new OutSock(this, {&kOut, [](auto){}}));

    auto receiver = [out = out_[0], mem = mem_](const auto& ctx, auto&&) {
      out->Send(ctx, Value(mem->data().value));
    };
    in_.emplace_back(new NodeLambdaInSock(this, {&kIn, [](auto){}}, std::move(receiver)));
  }

  Imm(Env* env, const msgpack::object& obj) :
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
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    const auto& data = mem_->data();
    return std::make_unique<Imm>(env, Value(data.value), data.size);
  }

  void UpdateTree(RefStack&) noexcept override;
  void UpdateNode(RefStack&, const std::shared_ptr<Editor>&) noexcept override;
  void UpdateTypeChanger(bool mini = false) noexcept;
  void UpdateEditor() noexcept;

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


  void OnUpdate() noexcept {
    lastmod_ = Clock::now();
  }
};
void Imm::UpdateTree(RefStack&) noexcept {
  UpdateTypeChanger();
  ImGui::SameLine();
  UpdateEditor();
}
void Imm::UpdateNode(RefStack&, const std::shared_ptr<Editor>& ctx) noexcept {
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

} }  // namespace kingtaker
