#include "kingtaker.hh"

#include <algorithm>
#include <chrono>
#include <optional>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <ImNodes.h>
#include <implot.h>

#include "iface/node.hh"

#include "util/gui.hh"
#include "util/value.hh"


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
      gui::NodeSocket();
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

  ImmValue(Value&& v = Value::Integer{0}, ImVec2 size = {0, 0}) noexcept :
      File(&type_), Node(kNone), value_(std::move(v)), size_(size) {
    out_.emplace_back(new Emitter(this));
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj) {
    const auto value = Value::Deserialize(msgpack::find(obj, "value"s));
    const auto size  = msgpack::find(obj, "size"s).as<std::pair<float, float>>();
    return std::make_unique<ImmValue>(Value(value), ImVec2 {size.first, size.second});
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(2);

    pk.pack("size");
    pk.pack(std::make_pair(size_.x, size_.y));

    pk.pack("value"s);
    value_.Serialize(pk);
  }
  std::unique_ptr<File> Clone() const noexcept override {
    return std::make_unique<ImmValue>(Value(value_), size_);
  }

  void Update(RefStack&, const std::shared_ptr<Context>& ctx) noexcept override {
    const auto em = ImGui::GetFontSize();
    const auto fh = ImGui::GetFrameHeight();
    const auto sp = ImGui::GetStyle().ItemSpacing.y - .4f;

    ImGui::TextUnformatted("IMM");
    auto& v = value_;

    bool mod = false;
    const char* type =
        v.has<Value::Integer>()? "Int":
        v.has<Value::Scalar>()?  "Sca":
        v.has<Value::Boolean>()? "Boo":
        v.has<Value::Vec2>()?    "Ve2":
        v.has<Value::Vec3>()?    "Ve3":
        v.has<Value::Vec4>()?    "Ve4":
        v.has<Value::String>()?  "Str": "XXX";
    ImGui::Button(type);

    gui::NodeCanvasResetZoom();
    if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
      if (ImGui::MenuItem("integer", nullptr, v.has<Value::Integer>())) {
        v   = Value::Integer {0};
        mod = true;
      }
      if (ImGui::MenuItem("scalar", nullptr, v.has<Value::Scalar>())) {
        v   = Value::Scalar {0};
        mod = true;
      }
      if (ImGui::MenuItem("boolean", nullptr, v.has<Value::Boolean>())) {
        v   = Value::Boolean {false};
        mod = true;
      }
      if (ImGui::MenuItem("vec2", nullptr, v.has<Value::Vec2>())) {
        v   = Value::Vec2 {0., 0.};
        mod = true;
      }
      if (ImGui::MenuItem("vec3", nullptr, v.has<Value::Vec3>())) {
        v   = Value::Vec3 {0., 0., 0.};
        mod = true;
      }
      if (ImGui::MenuItem("vec4", nullptr, v.has<Value::Vec4>())) {
        v   = Value::Vec4 {0., 0., 0., 0.};
        mod = true;
      }
      if (ImGui::MenuItem("string", nullptr, v.has<Value::String>())) {
        v   = ""s;
        mod = true;
      }
      ImGui::EndPopup();
    }
    gui::NodeCanvasSetZoom();

    ImGui::SameLine();
    if (v.has<Value::Integer>()) {
      gui::ResizeGroup _("##ResizeGroup", &size_, {4, fh/em}, {12, fh/em}, em);
      ImGui::SetNextItemWidth(size_.x*em);
      mod = ImGui::DragScalar("##InputValue", ImGuiDataType_S64, &v.getUniq<Value::Integer>());

    } else if (v.has<Value::Scalar>()) {
      gui::ResizeGroup _("##ResizeGroup", &size_, {4, fh/em}, {12, fh/em}, em);
      ImGui::SetNextItemWidth(size_.x*em);
      mod = ImGui::DragScalar("##InputValue", ImGuiDataType_Double, &v.getUniq<Value::Scalar>());

    } else if (v.has<Value::Boolean>()) {
      mod = ImGui::Checkbox("##InputValue", &v.getUniq<Value::Boolean>());

    } else if (v.has<Value::Vec2>()) {
      const auto h = (2*fh + sp)/em;
      gui::ResizeGroup _("##ResizeGroup", &size_, {4, h}, {12, h}, em);
      mod = UpdateVec(v.getUniq<Value::Vec2>());

    } else if (v.has<Value::Vec3>()) {
      const auto h = (3*fh + 2*sp)/em;
      gui::ResizeGroup _("##ResizeGroup", &size_, {4, h}, {12, h}, em);
      mod = UpdateVec(v.getUniq<Value::Vec3>());

    } else if (v.has<Value::Vec4>()) {
      const auto h = (4*fh + 3*sp)/em;
      gui::ResizeGroup _("##ResizeGroup", &size_, {4, h}, {12, h}, em);
      mod = UpdateVec(v.getUniq<Value::Vec4>());

    } else if (v.has<Value::String>()) {
      gui::ResizeGroup _("##ResizeGroup", &size_, {4, 4}, {24, 24*em}, em);
      mod = ImGui::InputTextMultiline("##InputValue", &v.getUniq<Value::String>(), size_*em);

    } else {
      assert(false);
    }
    if (mod) {
      out_[0]->Send(ctx, Value(v));
      lastmod_ = Clock::now();
    }

    ImGui::SameLine();
    if (ImNodes::BeginOutputSlot("out", 1)) {
      gui::NodeSocket();
      ImNodes::EndSlot();
    }
  }
  template <int D>
  bool UpdateVec(linalg::vec<double, D>& vec) {
    bool mod = false;
    for (int i = 0; i < D; ++i) {
      ImGui::PushID(&vec[i]);
      ImGui::SetNextItemWidth(size_.x*ImGui::GetFontSize());
      if (ImGui::DragScalar("##InputValue", ImGuiDataType_Double, &vec[i])) {
        mod = true;
      }
      ImGui::PopID();
    }
    return mod;
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

  ImVec2 size_;

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

  Oscilloscope(size_t n = 1, ImVec2 size = {0, 0}) noexcept :
      File(&type_), Node(kMenu),
      data_(std::make_shared<Data>()), size_(size) {
    assert(n > 0);

    in_.emplace_back(std::make_shared<PulseReceiver>(this, "CLK"));
    for (size_t i = 0; i < n; ++i) AddSock();
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj) {
    auto size = msgpack::find(obj, "size"s).as<std::pair<float, float>>();
    return std::make_unique<Oscilloscope>(
        msgpack::find(obj, "count"s).as<size_t>(),
        ImVec2 {size.first, size.second});
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(2);

    pk.pack("count"s);
    pk.pack(data_->streams.size());

    pk.pack("size");
    pk.pack(std::make_tuple(size_.x, size_.y));
  }
  std::unique_ptr<File> Clone() const noexcept override {
    return std::make_unique<Oscilloscope>(data_->streams.size(), size_);
  }

  bool AddSock() noexcept {
    const size_t n = data_->streams.size();
    if (n >= 20) return false;

    std::string name = "A";
    name[0] += static_cast<char>(n);

    auto st = std::make_shared<Stream>(name.c_str());
    data_->streams.push_back(st);

    auto vrecv = std::make_shared<CachedInSock>(this, st->inName(), Value::Pulse());
    in_.emplace_back(std::make_shared<StreamPulseReceiver>(this, st->clkName(), vrecv, st));
    in_.push_back(vrecv);
    return true;
  }
  void RemoveSock() noexcept {
    if (data_->streams.size() <= 1) return;

    data_->streams.pop_back();
    in_.pop_back();
    in_.pop_back();
  }

  void Update(RefStack&, const std::shared_ptr<Context>&) noexcept override {
    ImGui::Text("OSCILLO");

    const auto em = ImGui::GetFontSize();

    if (ImNodes::BeginInputSlot("CLK", 1)) {
      gui::NodeSocket();
      ImNodes::EndSlot();
    }
    ImGui::SameLine();
    ImGui::Text("CLK: %zu", data_->clk);

    ImGui::BeginGroup();
    for (const auto& st : data_->streams) {
      ImGui::SetCursorPosY(ImGui::GetCursorPosY()+em*.25f);
      for (auto v : {st->clkName().c_str(), st->inName().c_str()}) {
        if (ImNodes::BeginInputSlot(v, 1)) {
          gui::NodeSocket();
          ImNodes::EndSlot();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(v);
      }
    }
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::BeginGroup();
    {
      gui::ResizeGroup _("##Resizer", &size_, {16, 16}, {64, 64}, em);

      if (ImPlot::BeginPlot("##Graph", size_*em)) {
        ImPlot::SetupAxes(nullptr, nullptr,
                          ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        for (auto& st : data_->streams) st->PlotLine();
        ImPlot::EndPlot();
      }
    }
    ImGui::EndGroup();
  }
  void UpdateMenu(RefStack&, const std::shared_ptr<Context>&) noexcept override {
    if (ImGui::MenuItem("Add socket")) {
      AddSock();
    }
    if (ImGui::MenuItem("Remove socket")) {
      RemoveSock();
    }
  }

  Time lastModified() const noexcept override { return {}; }

  void* iface(const std::type_index& t) noexcept {
    if (t == typeid(iface::Node)) return static_cast<iface::Node*>(this);
    return nullptr;
  }

 private:
  class Stream final {
   public:
    Stream(std::string_view n) noexcept :
        name_(n), clk_name_(name_+"_CLK"), in_name_(name_+"_IN") {
    }

    void Add(double x, double y) noexcept {
      if (x_.size() && x_.back() == x) {
        y_.back() = y;
      } else {
        x_.push_back(x);
        y_.push_back(y);
      }
    }
    void Clear() noexcept {
      x_.clear();
      y_.clear();
    }

    void PlotLine() const noexcept {
      ImPlot::PlotLine(name_.c_str(), &x_[0], &y_[0], static_cast<int>(x_.size()));
    }

    const std::string& name() const noexcept { return name_; }

    const std::string& clkName() const noexcept { return clk_name_; }
    const std::string& inName() const noexcept { return in_name_; }

   private:
    std::string name_;
    std::vector<double> x_;
    std::vector<double> y_;

    const std::string clk_name_;
    const std::string in_name_;
  };
  class Data final {
   public:
    std::vector<std::shared_ptr<Stream>> streams;

    size_t clk = 0;
  };

  class PulseReceiver : public InSock {
   public:
    PulseReceiver(Oscilloscope* o, std::string_view n) :
        InSock(o, n), data_(o->data_) {
    }

    void Receive(const std::shared_ptr<Context>&, Value&&) noexcept override {
      auto data = data_.lock();
      if (!data) return;
      ++data->clk;
    }

   private:
    std::weak_ptr<Data> data_;
  };
  class StreamPulseReceiver : public InSock {
   public:
    StreamPulseReceiver(Oscilloscope* o,
                        std::string_view n,
                        const std::shared_ptr<CachedInSock>& v,
                        const std::shared_ptr<Stream>& st) :
        InSock(o, n), data_(o->data_), vsock_(v), st_(st) {
    }

    void Receive(const std::shared_ptr<Context>& ctx, Value&& p) noexcept override {
      auto data  = data_.lock();
      auto vsock = vsock_.lock();
      auto st    = st_.lock();
      if (!data || !vsock || !st) return;

      double sca = 0.;

      const auto& v = vsock->value();
      if (v.has<Value::Scalar>()) {
        sca = v.get<Value::Scalar>();
      } else if (v.has<Value::Integer>()) {
        sca = static_cast<double>(v.get<Value::Integer>());
      } else {
        return;
      }
      st->Add(static_cast<double>(data->clk), sca);

      InSock::Receive(ctx, std::move(p));
    }

   private:
    std::weak_ptr<Data> data_;

    std::weak_ptr<CachedInSock> vsock_;

    std::weak_ptr<Stream> st_;
  };

  std::shared_ptr<Data> data_;

  ImVec2 size_;
};

} }  // namespace kingtaker
