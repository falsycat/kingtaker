#include "kingtaker.hh"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <fstream>
#include <optional>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <ImNodes.h>
#include <implot.h>

#include "iface/dir.hh"
#include "iface/factory.hh"
#include "iface/gui.hh"
#include "iface/node.hh"

#include "util/gui.hh"
#include "util/ptr_selector.hh"
#include "util/value.hh"


namespace kingtaker {
namespace {

class ImmValue : public File, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<ImmValue>(
      "ImmValue", "immediate value",
      {typeid(iface::Node)});

  ImmValue(const std::shared_ptr<Env>& env, Value&& v = Value::Integer{0}, ImVec2 size = {0, 0}) noexcept :
      File(&type_, env), Node(kNone), value_(std::move(v)), size_(size) {
    out_.emplace_back(new Emitter(this));
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    const auto value = Value::Deserialize(msgpack::find(obj, "value"s));
    const auto size  = msgpack::find(obj, "size"s).as<std::pair<float, float>>();
    return std::make_unique<ImmValue>(env, Value(value), ImVec2 {size.first, size.second});
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(2);

    pk.pack("size");
    pk.pack(std::make_pair(size_.x, size_.y));

    pk.pack("value"s);
    value_.Serialize(pk);
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<ImmValue>(env, Value(value_), size_);
  }

  void Update(RefStack&, const std::shared_ptr<Context>& ctx) noexcept override {
    const auto em = ImGui::GetFontSize();
    const auto fh = ImGui::GetFrameHeight();
    const auto sp = ImGui::GetStyle().ItemSpacing.y - .4f;

    ImGui::TextUnformatted("IMM");
    auto& v = value_;

    bool mod = false;
    const char* type =
        v.has<Value::Pulse>()?   "Pul":
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
      if (ImGui::MenuItem("pulse", nullptr, v.has<Value::Pulse>())) {
        v   = Value::Pulse();
        mod = true;
      }
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
    if (v.has<Value::Pulse>()) {
      mod = ImGui::Button("Z");

    } else if (v.has<Value::Integer>()) {
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
      gui::ResizeGroup _("##ResizeGroup", &size_, {4, 4}, {24, 24}, em);
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
    return PtrSelector<iface::Node>(t).Select(this);
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

class ExternalText final : public File,
    public iface::DirItem,
    public iface::GUI,
    public iface::Factory<Value> {
 public:
  static inline TypeInfo type_ = TypeInfo::New<ExternalText>(
      "ExternalText", "text data from a native file",
      {typeid(iface::DirItem), typeid(iface::GUI), typeid(iface::Factory<Value>)});

  ExternalText(const std::shared_ptr<Env>& env, const std::string& path = "", bool editor_shown = false) noexcept :
      File(&type_, env), DirItem(kMenu),
      path_(path), editor_shown_(editor_shown),
      str_(std::make_shared<std::string>()) {
    Load();
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj, const std::shared_ptr<Env>& env) {
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

  void Update(RefStack& ref) noexcept override {
    const auto em = ImGui::GetFontSize();

    if (editor_shown_) {
      const auto id = ref.Stringify() + ": Text Editor";
      ImGui::SetNextWindowSize({16*em, 16*em}, ImGuiCond_FirstUseEver);
      if (ImGui::Begin(id.c_str(), &editor_shown_, ImGuiWindowFlags_MenuBar)) {
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
      ImGui::End();
    }
  }
  void UpdateMenu(RefStack&) noexcept override {
    ImGui::MenuItem("Text Editor", nullptr, &editor_shown_);
  }

  Time lastModified() const noexcept override { return lastmod_; }

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::DirItem, iface::GUI, iface::Factory<Value>>(t).Select(this);
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
  }
  bool Load(const std::string& path = "") noexcept {
    const auto& p = path.size()? path: path_;

    // TODO(falsycat): make async
    std::ifstream ifs(p);
    auto str = std::make_shared<std::string>(
        std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());

    input_path_load_failure_ = false;
    if (ifs.fail()) {
      input_path_load_failure_ = true;
      return false;
    }

    lastmod_ = Clock::now();

    modified_     = false;
    save_failure_ = false;

    path_ = p;
    str_  = std::move(str);
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

} }  // namespace kingtaker
