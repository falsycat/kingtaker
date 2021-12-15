#define IMGUI_DEFINE_MATH_OPERATORS

#include "kingtaker.hh"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <ImNodes.h>

#include "iface/gui.hh"
#include "iface/node.hh"


namespace kingtaker {

static void UpdatePin() noexcept {
  auto dlist = ImGui::GetWindowDrawList();

  const auto radius = ImGui::GetFontSize()/2 / ImNodes::CanvasState().Zoom;
  const auto radvec = ImVec2(radius, radius);
  const auto pos    = ImGui::GetCursorScreenPos();

  dlist->AddCircleFilled(
      pos+radvec, radius, IM_COL32(100, 100, 100, 100));
  dlist->AddCircleFilled(
      pos+radvec, radius*.8f, IM_COL32(200, 200, 200, 200));

  ImGui::SetCursorPos(ImGui::GetCursorPos() + radvec*2);
}


class PulseValue : public File, public iface::Node {
 public:
  static inline TypeInfo* type_ = TypeInfo::New<PulseValue>(
      "PulseValue", "pulse emitter");

  PulseValue() :
      File(type_), Node({}, {&out_}), out_(this), gui_(this) {
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object&) {
    return std::make_unique<PulseValue>();
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_nil();
  }

  std::unique_ptr<File> Clone() const noexcept override {
    return std::make_unique<PulseValue>();
  }

  Time lastModified() const noexcept override {
    return {};
  }
  void* iface(const std::type_index& t) noexcept override {
    if (t == typeid(iface::Node)) return static_cast<iface::Node*>(this);
    if (t == typeid(iface::GUI))  return &gui_;
    return nullptr;
  }

 private:
  class PulseEmitter : public OutSock {
   public:
    PulseEmitter(PulseValue* o) : OutSock(o, "out"), owner_(o) {
    }

    void Send(Value&& v) noexcept override {
      OutSock::Send(std::move(v));
    }

   private:
    PulseValue* owner_;
  } out_;

  class GUI : public iface::GUI {
   public:
    GUI(PulseValue* o) : iface::GUI(kNode), owner_(o) {
    }

    void UpdateNode(RefStack&) noexcept override {
      ImGui::TextUnformatted("PULSE");

      const auto& style = ImGui::GetStyle();

      if (ImGui::Button("Z")) {
        owner_->out_.Send(Pulse());
      }

      ImGui::SameLine();
      ImGui::SetCursorPosY(ImGui::GetCursorPosY()+style.ItemInnerSpacing.y);
      if (ImNodes::BeginOutputSlot("out", 1)) {
        UpdatePin();
        ImNodes::EndSlot();
      }
    }

   private:
    PulseValue* owner_;
  } gui_;
};

class ImmValue : public File, public iface::Node {
 public:
  static inline TypeInfo* type_ = TypeInfo::New<ImmValue>(
      "ImmValue", "immediate value");

  ImmValue(Value&& v = Integer{0}) :
      File(type_), Node({}, {&out_}), value_(std::move(v)), out_(this), gui_(this) {
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj) {
    return std::make_unique<ImmValue>(Value::Deserialize(obj));
  }
  void Serialize(Packer& pk) const noexcept override {
    value_.Serialize(pk);
  }

  std::unique_ptr<File> Clone() const noexcept override {
    return std::make_unique<ImmValue>(Value(value_));
  }

  Time lastModified() const noexcept override {
    return lastmod_;
  }
  void* iface(const std::type_index& t) noexcept override {
    if (t == typeid(iface::Node)) return static_cast<iface::Node*>(this);
    if (t == typeid(iface::GUI))  return &gui_;
    return nullptr;
  }

 private:
  Time lastmod_;

  Value value_;

  class Emitter final : public CachedOutSock {
   public:
    Emitter(ImmValue* o) noexcept : CachedOutSock(o, "out", Integer{0}) { }
  } out_;

  class GUI final : public iface::GUI {
   public:
    GUI(ImmValue* o) noexcept : iface::GUI(kNode), owner_(o) { }

    void UpdateNode(RefStack&) noexcept override {
      const auto  em    = ImGui::GetFontSize();
      const auto& style = ImGui::GetStyle();

      ImGui::TextUnformatted("IMM");
      auto& v = owner_->value_;

      const char* type =
          v.has<Integer>()? "Int":
          v.has<Scalar>()?  "Sca":
          v.has<Boolean>()? "Boo":
          v.has<String>()?  "Str": "XXX";
      ImGui::Button(type);
      if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
        if (ImGui::MenuItem("integer", nullptr, v.has<Integer>())) {
          v = Integer {0};
        }
        if (ImGui::MenuItem("scalar", nullptr, v.has<Scalar>())) {
          v = Scalar {0};
        }
        if (ImGui::MenuItem("boolean", nullptr, v.has<Boolean>())) {
          v = Boolean {false};
        }
        if (ImGui::MenuItem("string", nullptr, v.has<String>())) {
          v = ""s;
        }
        ImGui::EndCombo();
      }

      ImGui::SameLine();
      bool mod = false;
      if (v.has<Integer>()) {
        ImGui::SetNextItemWidth(6*em);
        mod = ImGui::DragScalar("##InputValue", ImGuiDataType_S64, &v.getUniq<Integer>());
      } else if (v.has<Scalar>()) {
        ImGui::SetNextItemWidth(8*em);
        mod = ImGui::DragScalar("##InputValue", ImGuiDataType_Double, &v.getUniq<Scalar>());
      } else if (v.has<Boolean>()) {
        mod = ImGui::Checkbox("##InputValue", &v.getUniq<Boolean>());

      } else if (v.has<String>()) {
        mod = ImGui::InputTextMultiline("##InputValue", &v.getUniq<String>(), {8*em, 4*em});
      } else {
        assert(false);
      }
      if (mod) {
        owner_->out_.Send(Value(owner_->value_));
        owner_->lastmod_ = Clock::now();
      }

      ImGui::SameLine();
      ImGui::SetCursorPosY(ImGui::GetCursorPosY()+style.ItemInnerSpacing.y);
      if (ImNodes::BeginOutputSlot("out", 1)) {
        UpdatePin();
        ImNodes::EndSlot();
      }
    }

   private:
    ImmValue* owner_;
  } gui_;
};

}  // namespace kingtaker
