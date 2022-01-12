#include "kingtaker.hh"

#include <algorithm>
#include <chrono>
#include <optional>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <ImNodes.h>

#include "iface/node.hh"


namespace kingtaker {
namespace {

class PulseValue : public File, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<PulseValue>(
      "PulseValue", "pulse emitter",
      {typeid(iface::Node)});

  PulseValue() : File(&type_), Node(kNone) {
    out_.emplace_back(new PulseEmitter(this));
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

  void Update(RefStack&, const std::shared_ptr<Context>& ctx) noexcept override {
    ImGui::TextUnformatted("PULSE");

    if (ImGui::Button("Z")) {
      out_[0]->Send(ctx, Value::Pulse());
    }

    ImGui::SameLine();
    if (ImNodes::BeginOutputSlot("out", 1)) {
      UpdatePin();
      ImNodes::EndSlot();
    }
  }

  Time lastModified() const noexcept override {
    return {};
  }
  void* iface(const std::type_index& t) noexcept override {
    if (t == typeid(iface::Node)) return static_cast<iface::Node*>(this);
    return nullptr;
  }

 private:
  class PulseEmitter : public OutSock {
   public:
    PulseEmitter(PulseValue* o) : OutSock(o, "out") {
    }
  };
};

class ImmValue : public File, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<ImmValue>(
      "ImmValue", "immediate value",
      {typeid(iface::Node)});

  ImmValue(Value&& v = Value::Integer{0}) :
      File(&type_), Node(kNone), value_(std::move(v)) {
    out_.emplace_back(new Emitter(this));
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

  void Update(RefStack&, const std::shared_ptr<Context>& ctx) noexcept override {
    const auto em = ImGui::GetFontSize();

    ImGui::TextUnformatted("IMM");
    auto& v = value_;

    const char* type =
        v.has<Value::Integer>()? "Int":
        v.has<Value::Scalar>()?  "Sca":
        v.has<Value::Boolean>()? "Boo":
        v.has<Value::String>()?  "Str": "XXX";
    ImGui::Button(type);
    if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
      if (ImGui::MenuItem("integer", nullptr, v.has<Value::Integer>())) {
        v = Value::Integer {0};
      }
      if (ImGui::MenuItem("scalar", nullptr, v.has<Value::Scalar>())) {
        v = Value::Scalar {0};
      }
      if (ImGui::MenuItem("boolean", nullptr, v.has<Value::Boolean>())) {
        v = Value::Boolean {false};
      }
      if (ImGui::MenuItem("string", nullptr, v.has<Value::String>())) {
        v = ""s;
      }
      ImGui::EndPopup();
    }

    ImGui::SameLine();
    bool mod = false;
    if (v.has<Value::Integer>()) {
      ImGui::SetNextItemWidth(6*em);
      mod = ImGui::DragScalar("##InputValue", ImGuiDataType_S64, &v.getUniq<Value::Integer>());

    } else if (v.has<Value::Scalar>()) {
      ImGui::SetNextItemWidth(8*em);
      mod = ImGui::DragScalar("##InputValue", ImGuiDataType_Double, &v.getUniq<Value::Scalar>());

    } else if (v.has<Value::Boolean>()) {
      mod = ImGui::Checkbox("##InputValue", &v.getUniq<Value::Boolean>());

    } else if (v.has<Value::String>()) {
      mod = ImGui::InputTextMultiline("##InputValue", &v.getUniq<Value::String>(), {8*em, 4*em});

    } else {
      assert(false);
    }
    if (mod) {
      out_[0]->Send(ctx, Value(v));
      lastmod_ = Clock::now();
    }

    ImGui::SameLine();
    if (ImNodes::BeginOutputSlot("out", 1)) {
      UpdatePin();
      ImNodes::EndSlot();
    }
  }

  Time lastModified() const noexcept override {
    return lastmod_;
  }
  void* iface(const std::type_index& t) noexcept override {
    if (t == typeid(iface::Node)) return static_cast<iface::Node*>(this);
    return nullptr;
  }

 private:
  Time lastmod_;

  Value value_;

  class Emitter final : public CachedOutSock {
   public:
    Emitter(ImmValue* o) noexcept : CachedOutSock(o, "out", Value::Integer{0}) { }
  };
};

class Oscilloscope : public File, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<Oscilloscope>(
      "Oscilloscope", "value inspector",
      {typeid(iface::Node)});

  Oscilloscope() : File(&type_), Node(kNone), life_(std::make_shared<std::monostate>()) {
    in_.emplace_back(std::make_shared<Receiver>(this));
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object&) {
    return std::make_unique<Oscilloscope>();
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_nil();
  }
  std::unique_ptr<File> Clone() const noexcept override {
    return std::make_unique<Oscilloscope>();
  }

  void Update(RefStack&, const std::shared_ptr<Context>&) noexcept override {
    ImGui::TextUnformatted("OSCILLO");

    const auto em = ImGui::GetFontSize();
    ImGui::PushItemWidth(8*em);

    if (ImNodes::BeginInputSlot("in", 1)) {
      UpdatePin();
      ImNodes::EndSlot();
    }

    ImGui::SameLine();
    ImGui::Text(msg_.c_str());

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.f, 0.f});

    const auto& v    = values_;
    const auto  size = ImVec2 {12*em, 4*em};
    if (ImGui::BeginChild("graph", size, true)) {
      auto dlist = ImGui::GetWindowDrawList();

      const auto offset = ImGui::GetCursorScreenPos();
      const auto now    = Clock::now();

      Value::Scalar min = 0, max = 0;
      for (auto itr = v.rbegin(); itr < v.rend(); ++itr) {
        const auto t = std::chrono::
            duration_cast<std::chrono::milliseconds>(now - itr->first);
        if (t > std::chrono::seconds(1)) break;

        const auto& y = itr->second;

        std::optional<Value::Scalar> v;
        if (y.has<Value::Integer>()) v = y.get<Value::Integer>();
        if (y.has<Value::Scalar>())  v = y.get<Value::Scalar>();

        if (v) {
          min = std::min(min, *v);
          max = std::max(max, *v);
        }
      }

      auto vsize = max-min;
      min -= vsize*.2;
      max += vsize*.2;
      vsize = max-min;

      float prev = offset.x;
      for (auto itr = v.rbegin(); itr < v.rend(); ++itr) {
        const auto t = std::chrono::
            duration_cast<std::chrono::milliseconds>(now - itr->first);
        if (t > std::chrono::seconds(1)) break;

        const float x = static_cast<float>(t.count())/1000.f;
        const auto& y = itr->second;

        std::optional<Value::Scalar> v;
        if (y.has<Value::Integer>()) v = y.get<Value::Integer>();
        if (y.has<Value::Scalar>())  v = y.get<Value::Scalar>();

        const float px = (1-x)*size.x;
        dlist->AddLine(ImVec2 {px, 0} + offset,
                       ImVec2 {px, size.y} + offset,
                       IM_COL32(100, 100, 100, 255));

        if (vsize > 0 && v) {
          const float py = static_cast<float>((1 - (*v-min) / vsize) * size.y);
          dlist->AddLine(ImVec2 {prev, py} + offset,
                         ImVec2 {px, py} + offset,
                         IM_COL32(255, 255, 255, 255));
        }
        prev = px;
      }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(1);

    if (v.size()) {
      ImGui::BeginDisabled();
      auto last = v.back().second;
      if (last.has<Value::Integer>()) {
        ImGui::DragScalar("integer", ImGuiDataType_S64, &last.getUniq<Value::Integer>());
      }
      if (last.has<Value::Scalar>()) {
        ImGui::DragScalar("scalar", ImGuiDataType_Double, &last.getUniq<Value::Scalar>());
      }
      if (last.has<Value::Boolean>()) {
        ImGui::Checkbox("bool", &last.getUniq<Value::Boolean>());
      }
      if (last.has<Value::String>()) {
        auto str = last.get<Value::String>();
        ImGui::InputTextMultiline("string", &str, ImVec2 {0.f, 4*em});
      }
      ImGui::EndDisabled();
    }

    ImGui::PopItemWidth();
  }

  Time lastModified() const noexcept override { return {}; }

  void* iface(const std::type_index& t) noexcept {
    if (t == typeid(iface::Node)) return static_cast<iface::Node*>(this);
    return nullptr;
  }

 private:
  std::vector<std::pair<Time, Value>> values_;

  std::string msg_ = "waiting...";

  std::shared_ptr<std::monostate> life_;

  class Receiver : public InSock {
   public:
    Receiver(Oscilloscope* o) : InSock(o, "in"), owner_(o), life_(o->life_) {
    }

    void Receive(const std::shared_ptr<Context>& ctx, Value&& v) noexcept override {
      if (life_.expired()) return;

      owner_->msg_ = "ok :)";
      owner_->values_.emplace_back(Clock::now(), Value(v));
      InSock::Receive(ctx, std::move(v));
    }

   private:
    Oscilloscope* owner_;

    std::weak_ptr<std::monostate> life_;
  };
};

} }  // namespace kingtaker
