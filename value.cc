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
#include "iface/factory.hh"
#include "iface/node.hh"

#include "util/format.hh"
#include "util/gui.hh"
#include "util/node.hh"
#include "util/notify.hh"
#include "util/ptr_selector.hh"
#include "util/value.hh"


namespace kingtaker {
namespace {

class Imm final : public File,
    public iface::Factory<Value>,
    public iface::DirItem,
    public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<Imm>(
      "Value/Imm", "immediate value",
      {typeid(iface::Factory<Value>), typeid(iface::DirItem), typeid(iface::Node)});

  Imm(const std::shared_ptr<Env>& env, Value&& v = Value::Integer {0}, ImVec2 size = {0, 0}) noexcept :
      File(&type_, env), DirItem(DirItem::kTree), Node(Node::kNone),
      value_(std::make_shared<Value>(std::move(v))),
      size_(size) {
    out_.emplace_back(new OutSock(this, "out"));

    std::weak_ptr<OutSock> wout = out_[0];
    std::weak_ptr<Value>   wval = value_;
    auto receiver = [wout, wval](const auto& ctx, auto&&) {
      auto val = wval.lock();
      auto out = wout.lock();
      if (!val || !out) return;
      out->Send(ctx, Value(*val));
    };
    in_.emplace_back(new LambdaInSock(this, "CLK", std::move(receiver)));
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    const auto value = Value::Deserialize(msgpack::find(obj, "value"s));
    const auto size  = msgpack::find(obj, "size"s).as<std::pair<float, float>>();
    return std::make_unique<Imm>(env, Value(value), ImVec2 {size.first, size.second});
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(2);

    pk.pack("size");
    pk.pack(std::make_pair(size_.x, size_.y));

    pk.pack("value"s);
    value_->Serialize(pk);
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<Imm>(env, Value(*value_), size_);
  }

  Value Create() noexcept override {
    return *value_;
  }

  void UpdateTree(RefStack&) noexcept override;
  void Update(RefStack&, const std::shared_ptr<Context>&) noexcept override;
  void UpdateTypeChanger(bool mini = false) noexcept;
  void UpdateEditor() noexcept;
  template <int D> bool UpdateVec(linalg::vec<double, D>& vec) noexcept;

  Time lastmod() const noexcept override {
    return lastmod_;
  }
  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node, iface::DirItem, iface::Node>(t).Select(this);
  }

 private:
  // permanentized value
  std::shared_ptr<Value> value_;

  Time lastmod_;

  ImVec2 size_;


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
  auto& v = *value_;

  const char* type =
      v.isInteger()? "Int":
      v.isScalar()?  "Sca":
      v.isBoolean()? "Boo":
      v.isVec2()?    "Ve2":
      v.isVec3()?    "Ve3":
      v.isVec4()?    "Ve4":
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
    if (ImGui::MenuItem("vec2", nullptr, v.isVec2())) {
      v = Value::Vec2 {0., 0.};
      OnUpdate();
    }
    if (ImGui::MenuItem("vec3", nullptr, v.isVec3())) {
      v = Value::Vec3 {0., 0., 0.};
      OnUpdate();
    }
    if (ImGui::MenuItem("vec4", nullptr, v.isVec4())) {
      v = Value::Vec4 {0., 0., 0., 0.};
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
  const auto sp = ImGui::GetStyle().ItemSpacing.y - .4f;

  auto& v = *value_;

  ImGui::SameLine();
  if (v.isInteger()) {
    gui::ResizeGroup _("##resizer", &size_, {4, fh/em}, {12, fh/em}, em);
    ImGui::SetNextItemWidth(size_.x*em);
    if (ImGui::DragScalar("##editor", ImGuiDataType_S64, &v.integer())) {
      OnUpdate();
    }

  } else if (v.isScalar()) {
    gui::ResizeGroup _("##resizer", &size_, {4, fh/em}, {12, fh/em}, em);
    ImGui::SetNextItemWidth(size_.x*em);
    if (ImGui::DragScalar("##editor", ImGuiDataType_Double, &v.scalar())) {
      OnUpdate();
    }

  } else if (v.isBoolean()) {
    if (ImGui::Checkbox("##editor", &v.boolean())) {
      OnUpdate();
    }

  } else if (v.isVec2()) {
    const auto h = (2*fh + sp)/em;
    gui::ResizeGroup _("##resizer", &size_, {4, h}, {12, h}, em);
    if (UpdateVec(v.vec2())) {
      OnUpdate();
    }

  } else if (v.isVec3()) {
    const auto h = (3*fh + 2*sp)/em;
    gui::ResizeGroup _("##resizer", &size_, {4, h}, {12, h}, em);
    if (UpdateVec(v.vec3())) {
      OnUpdate();
    }

  } else if (v.isVec4()) {
    const auto h = (4*fh + 3*sp)/em;
    gui::ResizeGroup _("##resizer", &size_, {4, h}, {12, h}, em);
    if (UpdateVec(v.vec4())) {
      OnUpdate();
    }

  } else if (v.isString()) {
    gui::ResizeGroup _("##resizer", &size_, {4, fh/em}, {24, 24}, em);
    if (ImGui::InputTextMultiline("##editor", &v.stringUniq(), size_*em)) {
      OnUpdate();
    }

  } else {
    ImGui::TextUnformatted("UNKNOWN TYPE X(");
  }
}
template <int D>
bool Imm::UpdateVec(linalg::vec<double, D>& vec) noexcept {
  bool mod = false;
  for (int i = 0; i < D; ++i) {
    ImGui::PushID(&vec[i]);
    ImGui::SetNextItemWidth(size_.x*ImGui::GetFontSize());
    if (ImGui::DragScalar("##value", ImGuiDataType_Double, &vec[i])) {
      mod = true;
    }
    ImGui::PopID();
  }
  return mod;
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


class ExternalText final : public File,
    public iface::DirItem,
    public iface::Factory<Value> {
 public:
  static inline TypeInfo type_ = TypeInfo::New<ExternalText>(
      "Value/ExternalText", "text data from a native file",
      {typeid(iface::DirItem), typeid(iface::Factory<Value>)});

  ExternalText(const std::shared_ptr<Env>& env, const std::string& path = "", bool editor_shown = false) noexcept :
      File(&type_, env), DirItem(kMenu),
      path_(path), editor_shown_(editor_shown),
      str_(std::make_shared<std::string>()) {
    Load();
  }

  static std::unique_ptr<File> Deserialize(
      const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    return std::make_unique<ExternalText>(
        env,
        msgpack::find(obj, "path"s).as<std::string>(),
        msgpack::find(obj, "editor_shown"s).as<bool>());
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(2);

    pk.pack("path"s);
    pk.pack(path_);

    pk.pack("editor_shown");
    pk.pack(editor_shown_);
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<ExternalText>(env, path_);
  }

  Value Create() noexcept override {
    assert(str_);
    return Value(str_);
  }

  void Update(RefStack&, Event&) noexcept override;
  void UpdateMenu(RefStack&) noexcept override;

  Time lastmod() const noexcept override { return lastmod_; }

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::DirItem, iface::Factory<Value>>(t).Select(this);
  }

 private:
  void Save() noexcept {
    if (path_.empty()) return;

    std::ofstream ofs(path_);
    ofs << *str_;

    save_failure_ = false;
    if (ofs.fail()) {
      save_failure_ = true;
      return;
    }
    modified_ = false;
  }
  bool Load(const std::string& path = "") noexcept {
    input_path_load_failure_ = true;

    const auto& p      = path.size()? path: path_;
    const auto  target = env()->path().parent_path() / p;

    const auto xx = env()->path().c_str();
    (void) xx;

    std::string str;
    try {
      // TODO(falsycat): make async
      std::ifstream ifs(target, std::ios::binary);

      str = std::string(std::istreambuf_iterator<char>(ifs),
                        std::istreambuf_iterator<char>());
      if (ifs.fail()) return false;
    } catch (std::exception&) {
      return false;
    }

    lastmod_ = Clock::now();

    modified_     = false;
    save_failure_ = false;

    path_ = p;
    str_  = std::make_shared<std::string>(std::move(str));

    input_path_load_failure_ = false;
    return true;
  }

  // permanentized params
  std::string path_;
  bool        editor_shown_;

  // volatile params
  std::shared_ptr<std::string> str_;

  Time lastmod_;

  std::string input_path_;
  bool        input_path_load_failure_ = false;

  bool modified_     = false;
  bool save_failure_ = false;
};
void ExternalText::Update(RefStack& ref, Event& ev) noexcept {
  const auto em = ImGui::GetFontSize();

  if (editor_shown_) {
    ImGui::SetNextWindowSize({16*em, 16*em}, ImGuiCond_FirstUseEver);

    constexpr auto kWinFlags = ImGuiWindowFlags_MenuBar;
    if (gui::BeginWindow(this, "TextEditor", ref, ev, &editor_shown_, kWinFlags)) {
      if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
          if (ImGui::MenuItem("Save")) {
            Save();
          }
          if (ImGui::BeginMenu("Load")) {
            constexpr auto kPathFlags = ImGuiInputTextFlags_EnterReturnsTrue;
            constexpr auto kPathHint  = "path to native file";
            if (ImGui::InputTextWithHint("##InputPath", kPathHint, &input_path_, kPathFlags)) {
              if (Load(input_path_)) {
                input_path_              = "";
                input_path_load_failure_ = false;
                ImGui::CloseCurrentPopup();
              }
            }
            ImGui::SetKeyboardFocusHere(-1);

            if (input_path_load_failure_) {
              ImGui::Bullet();
              ImGui::TextUnformatted("load failure");
            }
            ImGui::EndMenu();
          }
          if (ImGui::MenuItem("Reload")) {
            Load();
          }
          ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
          ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
      }

      if (path_.empty()) {
        ImGui::TextUnformatted("(New File):");
      } else {
        ImGui::Text("%s:", path_.c_str());
      }
      if (modified_) {
        ImGui::SameLine();
        ImGui::Text("(modified)");
      }
      if (save_failure_) {
        ImGui::SameLine();
        ImGui::Text("(save error)");
      }

      if (1 != str_.use_count()) str_ = std::make_shared<std::string>(*str_);
      if (ImGui::InputTextMultiline("##Editor", str_.get(), {-FLT_MIN, -FLT_MIN})) {
        lastmod_  = Clock::now();
        modified_ = true;
      }
    }
    gui::EndWindow();
  }
}
void ExternalText::UpdateMenu(RefStack&) noexcept {
  ImGui::MenuItem("Text Editor", nullptr, &editor_shown_);
}

} }  // namespace kingtaker
