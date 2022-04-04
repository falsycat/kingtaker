#include "kingtaker.hh"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <fstream>
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
      v.has<Value::Integer>()? "Int":
      v.has<Value::Scalar>()?  "Sca":
      v.has<Value::Boolean>()? "Boo":
      v.has<Value::Vec2>()?    "Ve2":
      v.has<Value::Vec3>()?    "Ve3":
      v.has<Value::Vec4>()?    "Ve4":
      v.has<Value::String>()?  "Str": "XXX";
  mini? ImGui::SmallButton(type): ImGui::Button(type);

  gui::NodeCanvasResetZoom();
  if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
    if (ImGui::MenuItem("integer", nullptr, v.has<Value::Integer>())) {
      v = Value::Integer {0};
      OnUpdate();
    }
    if (ImGui::MenuItem("scalar", nullptr, v.has<Value::Scalar>())) {
      v = Value::Scalar {0};
      OnUpdate();
    }
    if (ImGui::MenuItem("boolean", nullptr, v.has<Value::Boolean>())) {
      v = Value::Boolean {false};
      OnUpdate();
    }
    if (ImGui::MenuItem("vec2", nullptr, v.has<Value::Vec2>())) {
      v = Value::Vec2 {0., 0.};
      OnUpdate();
    }
    if (ImGui::MenuItem("vec3", nullptr, v.has<Value::Vec3>())) {
      v = Value::Vec3 {0., 0., 0.};
      OnUpdate();
    }
    if (ImGui::MenuItem("vec4", nullptr, v.has<Value::Vec4>())) {
      v = Value::Vec4 {0., 0., 0., 0.};
      OnUpdate();
    }
    if (ImGui::MenuItem("string", nullptr, v.has<Value::String>())) {
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
  if (v.has<Value::Integer>()) {
    gui::ResizeGroup _("##resizer", &size_, {4, fh/em}, {12, fh/em}, em);
    ImGui::SetNextItemWidth(size_.x*em);
    if (ImGui::DragScalar("##editor", ImGuiDataType_S64, &v.getUniq<Value::Integer>())) {
      OnUpdate();
    }

  } else if (v.has<Value::Scalar>()) {
    gui::ResizeGroup _("##resizer", &size_, {4, fh/em}, {12, fh/em}, em);
    ImGui::SetNextItemWidth(size_.x*em);
    if (ImGui::DragScalar("##editor", ImGuiDataType_Double, &v.getUniq<Value::Scalar>())) {
      OnUpdate();
    }

  } else if (v.has<Value::Boolean>()) {
    if (ImGui::Checkbox("##editor", &v.getUniq<Value::Boolean>())) {
      OnUpdate();
    }

  } else if (v.has<Value::Vec2>()) {
    const auto h = (2*fh + sp)/em;
    gui::ResizeGroup _("##resizer", &size_, {4, h}, {12, h}, em);
    if (UpdateVec(v.getUniq<Value::Vec2>())) {
      OnUpdate();
    }

  } else if (v.has<Value::Vec3>()) {
    const auto h = (3*fh + 2*sp)/em;
    gui::ResizeGroup _("##resizer", &size_, {4, h}, {12, h}, em);
    if (UpdateVec(v.getUniq<Value::Vec3>())) {
      OnUpdate();
    }

  } else if (v.has<Value::Vec4>()) {
    const auto h = (4*fh + 3*sp)/em;
    gui::ResizeGroup _("##resizer", &size_, {4, h}, {12, h}, em);
    if (UpdateVec(v.getUniq<Value::Vec4>())) {
      OnUpdate();
    }

  } else if (v.has<Value::String>()) {
    gui::ResizeGroup _("##resizer", &size_, {4, fh/em}, {24, 24}, em);
    if (ImGui::InputTextMultiline("##editor", &v.getUniq<Value::String>(), size_*em)) {
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


class Logger final : public File, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<Logger>(
      "Value/Logger", "log values received from input",
      {typeid(iface::Node)});

  static constexpr size_t N = 256;  // max log items

  Logger(
      const std::shared_ptr<Env>& env,
      ImVec2 size      = {0, 0},
      bool auto_scroll = true,
      bool show_elapse = false) noexcept :
      File(&type_, env), Node(kNone),
      size_(size), auto_scroll_(auto_scroll), show_elapse_(show_elapse) {
    auto handler = [self = this](const auto& ctx, auto&& v) {
      auto data = ctx->template GetOrNew<ValueData>(self);
      data->updated = true;

      data->items[data->tail] = ValueItem::CreateItem(v);
      data->tail = (data->tail+1)%N;
      if (data->tail == data->head) {
        data->head = (data->head+1)%N;
      }
    };
    in_.emplace_back(new LambdaInSock(this, "in", std::move(handler)));
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    std::tuple<float, float> size;
    msgpack::find(obj, "size"s).convert(size);
    return std::make_unique<Logger>(
        env,
        ImVec2(std::get<0>(size), std::get<1>(size)),
        msgpack::find(obj, "auto_scroll"s).as<bool>(),
        msgpack::find(obj, "show_elapse"s).as<bool>());
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(3);

    pk.pack("size"s);
    pk.pack(std::make_tuple(size_.x, size_.y));

    pk.pack("auto_scroll");
    pk.pack(auto_scroll_);

    pk.pack("show_elapse");
    pk.pack(show_elapse_);
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<Logger>(
        env, size_, auto_scroll_, show_elapse_);
  }

  void Update(RefStack&, const std::shared_ptr<Context>& ctx) noexcept override;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node>(t).Select(this);
  }

 private:
  // permanentized params
  ImVec2 size_;

  bool auto_scroll_;
  bool show_elapse_;


  struct ValueItem final {
   public:
    Time time;

    std::string type;
    std::string value;
    std::string tooltip;

    static ValueItem CreateItem(const Value& val) noexcept {
      ValueItem ret;
      ret.time    = Clock::now();
      ret.type    = val.StringifyType();
      ret.tooltip = val.Stringify();
      ret.value   = ret.tooltip.substr(0, ret.tooltip.find('\n'));
      return ret;
    }
  };
  class ValueData final : public Context::Data {
   public:
    std::array<ValueItem, N> items;
    size_t head = 0, tail = 0;

    bool updated = false;
  };
};
void Logger::Update(File::RefStack&, const std::shared_ptr<Context>& ctx) noexcept {
  const auto now = Clock::now();
  const auto em  = ImGui::GetFontSize();

  auto data = ctx->GetOrNew<ValueData>(this);

  ImGui::TextUnformatted("VALUE LOGGER");

  if (ImNodes::BeginInputSlot("in", 1)) {
    gui::NodeSocket();
    ImNodes::EndSlot();
  }
  ImGui::SameLine();

  {
    gui::ResizeGroup _("##ResizeGroup", &size_, {20, 12}, {32, 24}, em);

    ImGui::Checkbox("auto-scroll", &auto_scroll_);
    ImGui::SameLine();
    ImGui::Checkbox("show-elapse", &show_elapse_);

    constexpr auto kFlags =
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_Hideable  |
        ImGuiTableFlags_ContextMenuInBody |
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_ScrollY;
    const auto table_size = size_*em - ImVec2(0, ImGui::GetFrameHeightWithSpacing());
    if (ImGui::BeginTable("log", 3, kFlags, table_size)) {
      ImGui::TableSetupColumn("type");
      ImGui::TableSetupColumn("value");
      ImGui::TableSetupColumn("time");
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableHeadersRow();

      for (size_t idx = data->head;; idx = (idx+1)%N) {
        if (idx == data->tail) {
          if (data->updated && auto_scroll_) {
            ImGui::SetScrollHereY();
            data->updated = false;
          }
          break;
        }
        const auto& item = data->items[idx];

        const auto elapse    = now - item.time;
        const auto elapse_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapse).count());

        const auto appear  = 1.f-powf(1.f-std::min(static_cast<float>(elapse_ms)/1000.f, 1.f), 2);
        const auto appear4 = ImVec4(appear, appear, appear, appear);

        const auto bg   = ImGui::GetStyleColorVec4(ImGuiCol_TableRowBg);
        const auto bg_h = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
        const auto bg_c = (bg-bg_h)*appear4 + bg_h;

        ImGui::TableNextRow();
        ImGui::TableSetBgColor(
            ImGuiTableBgTarget_RowBg0, ImGui::ColorConvertFloat4ToU32(bg_c));

        if (ImGui::TableSetColumnIndex(0)) {
          constexpr auto kSelectableFlags =
              ImGuiSelectableFlags_SpanAllColumns |
              ImGuiSelectableFlags_AllowItemOverlap;
          ImGui::Selectable(item.type.c_str(), false, kSelectableFlags);
        }
        if (ImGui::TableNextColumn()) {
          ImGui::TextUnformatted(item.value.c_str());
          if (item.tooltip.size() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", item.tooltip.c_str());
          }
        }
        if (ImGui::TableNextColumn()) {
          if (show_elapse_) {
            ImGui::TextUnformatted(StringifyDuration(elapse).c_str());
          } else {
            ImGui::TextUnformatted(StringifyTime(item.time).c_str());
          }
        }
      }
      ImGui::EndTable();
    }
  }
}

} }  // namespace kingtaker
