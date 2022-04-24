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

class Imm final : public File, public iface::DirItem, public iface::Node {
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
      mem_({this, std::move(v), size}),
      sock_out_(this, &kOut),
      sock_clk_(this, &kIn,
                [this](auto& ctx, auto&&) {
                  sock_out_.Send(ctx, Value(mem_.data().value));
                }) {
    in_  = {&sock_clk_};
    out_ = {&sock_out_};
  }

  Imm(Env* env, const msgpack::object& obj) :
      Imm(env,
          Value(msgpack::find(obj, "value"s)),
          msgpack::as_if<ImVec2>(msgpack::find(obj, "size"s), {0, 0})) {
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(2);

    const auto& data = mem_.data();

    pk.pack("size");
    pk.pack(std::make_pair(data.size.x, data.size.y));

    pk.pack("value"s);
    data.value.Serialize(pk);
  }
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    const auto& data = mem_.data();
    return std::make_unique<Imm>(env, Value(data.value), data.size);
  }

  void UpdateTree(RefStack&) noexcept override;
  void UpdateNode(RefStack&, const std::shared_ptr<Editor>&) noexcept override;
  void UpdateTypeChanger(bool mini = false) noexcept;
  void UpdateEditor() noexcept;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::DirItem, iface::Memento, iface::Node>(t).
        Select(this, &mem_);
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
  Memento mem_;

  OutSock          sock_out_;
  NodeLambdaInSock sock_clk_;


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
    gui::NodeSockPoint();
    ImGui::SameLine();
    if (ImGui::Button("CLK")) {
      Queue::main().Push([this, ctx]() { sock_clk_.Receive(ctx, {}); });
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
    gui::NodeSockPoint();
    ImNodes::EndSlot();
  }
}
void Imm::UpdateTypeChanger(bool mini) noexcept {
  auto& v = mem_.data().value;

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

  auto& data = mem_.data();
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

  if (!ImGui::IsAnyItemActive() && data != mem_.commitData()) {
    mem_.Commit();
  }
}


class Name final : public File, public iface::Node {
 public:
  static inline TypeInfo kType = TypeInfo::New<Name>(
      "Value/Name", "name",
      {typeid(iface::Memento), typeid(iface::Node)});

  static inline const SockMeta kOut = {
    .name = "out", .type = SockMeta::kNamedValue,
  };

  Name(Env* env, std::vector<std::string>&& n = {}) noexcept :
      File(&kType, env), Node(Node::kNone),
      memento_({this, std::move(n)}), out_sock_(this, &kOut) {
    Rebuild();
  }

  Name(Env* env, const msgpack::object& obj)
  try : Name(env, obj.as<std::vector<std::string>>()) {
  } catch (msgpack::type_error&) {
    throw DeserializeException("broken Value/Name");
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack(memento_.data().names);
  }
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    return std::make_unique<Name>(env, std::vector<std::string>(memento_.data().names));
  }

  void UpdateNode(RefStack&, const std::shared_ptr<Editor>&) noexcept override;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Memento, iface::Node>(t).
        Select(this, &memento_);
  }

 private:
  struct UniversalData final {
   public:
    UniversalData(Name* owner, std::vector<std::string>&& n) noexcept :
        names(std::move(n)), owner_(owner) {
    }
    void Restore(const UniversalData& other) {
      names = other.names;
      owner_->Rebuild();
    }

    // permanentized
    std::vector<std::string> names;

   private:
    Name* owner_;
  };
  SimpleMemento<UniversalData> memento_;

  // volatile
  std::vector<std::unique_ptr<InSock>> in_socks_;
  OutSock out_sock_;

  std::string new_name_;
  std::vector<std::string> names_editing_;


  void Rebuild() noexcept {
    const auto& udata = memento_.data();

    in_socks_.resize(udata.names.size());
    in_.resize(udata.names.size());
    for (size_t i = 0; i < udata.names.size(); ++i) {
      in_socks_[i] = std::make_unique<CustomInSock>(this, &out_sock_, udata.names[i]);
      in_[i]       = in_socks_[i].get();
    }
    out_ = {&out_sock_};

    names_editing_ = udata.names;
    NotifySockChange();
  }


  class CustomInSock final : public InSock {
   public:
    CustomInSock(Name* owner, OutSock* out, const std::string& name) noexcept :
        InSock(owner, &meta_), out_(out), meta_({.name = name}) {
    }
    void Receive(const std::shared_ptr<Context>& ctx, Value&& v) noexcept {
      out_->Send(ctx, Value::Tuple { meta_.name, std::move(v) });
    }
   private:
    OutSock* out_;
    SockMeta meta_;
  };
};
void Name::UpdateNode(RefStack&, const std::shared_ptr<Editor>&) noexcept {
  auto& udata  = memento_.data();
  auto& enames = names_editing_;

  const auto em = ImGui::GetFontSize();

  ImGui::TextUnformatted("NAME");

  ImGui::BeginGroup();
  ImGui::PushItemWidth(6*em);
  {
    int      id = 0;
    intmax_t i  = 0;
    for (; i < static_cast<intmax_t>(enames.size()); ++i) {
      ImGui::PushID(id++);
      auto& name = enames[static_cast<size_t>(i)];
      if (ImNodes::BeginInputSlot(name.c_str(), 1)) {
        ImGui::AlignTextToFramePadding();
        gui::NodeSockPoint();
        ImGui::SameLine();
        ImGui::InputText("##name", &name);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          if (name.empty()) {
            names_editing_.erase(enames.begin()+i);
            --i;
          } else {
            while (std::count(enames.begin(), enames.end(), name) >= 2) {
              name += "_";
            }
          }
          if (udata.names != enames) {
            udata.names = enames;
            memento_.Commit();
            Rebuild();
          }
        }
        ImNodes::EndSlot();
      }
      ImGui::PopID();
    }

    ImGui::PushID(id);
    ImGui::Dummy({em, em});
    ImGui::SameLine();
    ImGui::InputTextWithHint("##name", "new name", &new_name_);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      while (std::count(enames.begin(), enames.end(), new_name_) >= 1) {
        new_name_ += "_";
      }
      enames.push_back(new_name_);
      udata.names.push_back(std::move(new_name_));
      memento_.Commit();
      Rebuild();
    }
    ImGui::PopID();
  }
  ImGui::PopItemWidth();
  ImGui::EndGroup();

  ImGui::SameLine();
  if (ImNodes::BeginOutputSlot("out", 1)) {
    ImGui::AlignTextToFramePadding();
    gui::NodeSockPoint();
    ImNodes::EndSlot();
  }
}

} }  // namespace kingtaker
