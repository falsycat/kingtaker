#include "kingtaker.hh"

#include <atomic>
#include <thread>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <ImNodes.h>

#include "iface/dir.hh"
#include "iface/gui.hh"
#include "iface/node.hh"

#include "util/gui.hh"
#include "util/notify.hh"
#include "util/ptr_selector.hh"
#include "util/value.hh"


namespace kingtaker {
namespace {

class SystemImGuiConfig : public File {
 public:
  static inline TypeInfo type_ = TypeInfo::New<SystemImGuiConfig>(
      "SystemImGuiConfig", "save/restore ImGui config", {});

  SystemImGuiConfig(const std::shared_ptr<Env>& env) noexcept :
      File(&type_, env) {
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    if (obj.type == msgpack::type::STR) {
      const auto& str = obj.via.str;
      ImGui::LoadIniSettingsFromMemory(str.ptr, str.size);
    }
    return std::make_unique<SystemImGuiConfig>(env);
  }
  void Serialize(Packer& pk) const noexcept override {
    size_t n;
    const char* ini = ImGui::SaveIniSettingsToMemory(&n);
    pk.pack_str(static_cast<uint32_t>(n));
    pk.pack_str_body(ini, static_cast<uint32_t>(n));
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<SystemImGuiConfig>(env);
  }
};


class SystemLogger : public File, public iface::GUI {
 public:
  static inline TypeInfo type_ = TypeInfo::New<SystemLogger>(
      "SystemLogger", "logger window", {});

  static inline const auto kIdSuffix = ": SystemLogger";

  SystemLogger(const std::shared_ptr<Env>& env, bool shown = true) noexcept :
      File(&type_, env), shown_(shown) {
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    return std::make_unique<SystemLogger>(env, obj.as<bool>());
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack(shown_);
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<SystemLogger>(env, shown_);
  }

  void Update(RefStack& ref) noexcept override {
    const auto id = ref.Stringify() + kIdSuffix;
    if (ImGui::Begin(id.c_str(), &shown_)) {
      ImGui::InputTextWithHint("##filter", "filter", &filter_);
      ImGui::SameLine();
      ImGui::Checkbox("auto-scroll", &autoscroll_);

      notify::UpdateLogger(filter_, autoscroll_);
    }
    ImGui::End();
  }
  bool OnFocus(const RefStack& ref, size_t depth) noexcept override {
    if (ref.size() != depth) return false;

    const auto id = ref.Stringify() + kIdSuffix;
    ImGui::SetWindowFocus(id.c_str());
    shown_ = true;
    return true;
  }

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::GUI>(t).Select(this);
  }

 private:
  std::string filter_;

  bool shown_      = true;
  bool autoscroll_ = true;
};


class PulseGenerator final : public File, public iface::DirItem, public iface::GUI {
 public:
  static inline TypeInfo type_ = TypeInfo::New<PulseGenerator>(
      "PulseGenerator", "emits a clock pulse into a specific node",
      {typeid(iface::DirItem), typeid(iface::GUI)});

  static inline const auto kIdSuffix = ": PulseGenerator";

  PulseGenerator(const std::shared_ptr<Env>& env,
                 const std::string& path      = "",
                 const std::string& sock_name = "",
                 bool               shown     = false,
                 bool               enable    = false) noexcept :
      File(&type_, env), DirItem(kNone),
      nctx_(std::make_shared<iface::Node::Context>()),
      path_(path), sock_name_(sock_name), shown_(shown), enable_(enable) {
  }

  static std::unique_ptr<File> Deserialize(
      const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    try {
      return std::make_unique<PulseGenerator>(
          env,
          msgpack::find(obj, "path"s).as<std::string>(),
          msgpack::find(obj, "sock_name"s).as<std::string>(),
          msgpack::as_if<bool>(msgpack::find(obj, "shown"s), false),
          msgpack::as_if<bool>(msgpack::find(obj, "enable"s), false));
    } catch (msgpack::type_error&) {
      throw DeserializeException("broken PulseGenerator");
    }
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(4);

    pk.pack("path"s);
    pk.pack(path_);

    pk.pack("sock_name"s);
    pk.pack(sock_name_);

    pk.pack("shown"s);
    pk.pack(shown_);

    pk.pack("enable"s);
    pk.pack(enable_);
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<PulseGenerator>(env, path_, sock_name_, shown_, enable_);
  }

  bool OnFocus(const RefStack& ref, size_t) noexcept override {
    const auto id = ref.Stringify() + kIdSuffix;
    ImGui::SetWindowFocus(id.c_str());
    shown_ = true;
    return true;
  }

  void Update(RefStack& ref) noexcept override {
    if (enable_) Emit(ref);

    const auto id = ref.Stringify() + kIdSuffix;
    if (ImGui::Begin(id.c_str(), &shown_)) {
      UpdateEditor(ref);
    }
    ImGui::End();
  }
  void UpdateEditor(RefStack&) noexcept;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::DirItem, iface::GUI>(t).Select(this);
  }

 private:
  std::shared_ptr<iface::Node::Context> nctx_;


  // permanentized params
  std::string path_;
  std::string sock_name_;

  bool shown_;
  bool enable_;

  // volatile params
  std::string path_editing_;


  void Emit(const RefStack& ref) noexcept {
    try {
      auto n = File::iface<iface::Node>(&*ref.Resolve(path_));
      if (!n) throw Exception("target doesn't have Node interface");

      auto sock = n->FindIn(sock_name_);
      if (!sock) throw Exception("missing input socket, "+sock_name_);

      sock->Receive(nctx_, Value::Pulse());
    } catch (Exception& e) {
      notify::Warn(ref, e.msg());
      enable_ = false;
    }
  }
};
void PulseGenerator::UpdateEditor(RefStack& ref) noexcept {
  const auto em = ImGui::GetFontSize();
  const auto w  = 8*em;

  ImGui::PushItemWidth(w);
  {
    // path edit
    if (enable_) ImGui::BeginDisabled();
    {
      ImGui::Button(("-> "s+(path_.size()? path_: "(empty)"s)).c_str(), {w, 0});
      if (path_.size() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", path_.c_str());
      }
      if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
        gui::InputPathMenu(ref, &path_editing_, &path_);
        ImGui::EndPopup();
      }
    }
    if (enable_) ImGui::EndDisabled();

    // context clear button
    ImGui::SameLine();
    if (ImGui::Button("X")) {
      nctx_ = std::make_shared<iface::Node::Context>();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("clears execution context");

    // pulse emit button
    ImGui::SameLine();
    if (ImGui::Button("Z")) {
      Emit(ref);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("emits pulse manually");

    // params editor
    if (enable_) ImGui::BeginDisabled();
    {
      ImGui::InputText("socket name", &sock_name_);
    }
    if (enable_) ImGui::EndDisabled();
    ImGui::Checkbox("enable", &enable_);
  }
  ImGui::PopItemWidth();
}


class LogPrinter final : public File, public iface::Node, public iface::GUI {
 public:
  static inline TypeInfo type_ = TypeInfo::New<LogPrinter>(
      "LogPrinter", "retrieves key/mouse data",
      {typeid(iface::Node), typeid(iface::GUI)});

  LogPrinter(const std::shared_ptr<Env>& env,
             notify::Level      lv   = notify::kTrace,
             const std::string& msg  = "",
             ImVec2             size = {0, 0}) noexcept :
      File(&type_, env), Node(kNone),
      data_(std::make_shared<UniversalData>(lv, msg)), size_(size) {
    std::weak_ptr<UniversalData> wdata = data_;
    auto task = [self = this, wdata](const auto&, auto&&) {
      auto data = wdata.lock();
      if (!data) return;
      // TODO: replace '%' with the passed value
      notify::Push(
          {std::source_location::current(), data->lv,
          data->msg, File::Path(data->path), self});
    };
    in_.emplace_back(new LambdaInSock(this, "clk", std::move(task)));
  }

  static std::unique_ptr<File> Deserialize(
      const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    try {
      const auto size = msgpack::as_if<std::tuple<float, float>>(
          msgpack::find(obj, "size"s), {0.f, 0.f});

      return std::make_unique<LogPrinter>(
          env,
          IntToLevel(msgpack::as_if<int>(msgpack::find(obj, "level"s), 0)),
          msgpack::as_if<std::string>(msgpack::find(obj, "msg"s), ""s),
          ImVec2 {std::get<0>(size), std::get<1>(size)});
    } catch (msgpack::type_error&) {
      return std::make_unique<LogPrinter>(env);
    }
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(3);

    pk.pack("level"s);
    pk.pack(LevelToInt(data_->lv));

    pk.pack("msg"s);
    pk.pack(data_->msg);

    pk.pack("size"s);
    pk.pack(std::make_tuple(size_.x, size_.y));
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<LogPrinter>(env, data_->lv, data_->msg, size_);
  }

  void Update(RefStack&) noexcept override;
  void Update(RefStack&, const std::shared_ptr<Context>&) noexcept override;
  static void UpdateLevelCombo(notify::Level* lv) noexcept;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::GUI, iface::Node>(t).Select(this);
  }

 private:
  struct UniversalData {
    UniversalData(notify::Level l, const std::string& v) noexcept :
        lv(l), msg(v) {
    }
    notify::Level lv;
    std::string msg;

    File::Path path;
  };
  std::shared_ptr<UniversalData> data_;

  ImVec2 size_;


  static int LevelToInt(notify::Level lv) noexcept {
    switch (lv) {
    case notify::kTrace: return 0;
    case notify::kInfo:  return 1;
    case notify::kWarn:  return 2;
    case notify::kError: return 3;
    default: assert(false); return 0;
    }
  }
  static notify::Level IntToLevel(int idx) noexcept {
    switch (idx) {
    case 0: return notify::kTrace;
    case 1: return notify::kInfo;
    case 2: return notify::kWarn;
    case 3: return notify::kError;
    default: return notify::kTrace;
    }
  }
};
void LogPrinter::Update(RefStack& ref) noexcept {
  data_->path = ref.GetFullPath();
}
void LogPrinter::Update(RefStack&, const std::shared_ptr<Context>&) noexcept {
  const auto em = ImGui::GetFontSize();
  const auto fh = ImGui::GetFrameHeight();

  ImGui::BeginGroup();
  {
    if (ImNodes::BeginInputSlot("clk", 1)) {
      gui::NodeSocket();
      ImNodes::EndSlot();
    }
  }
  ImGui::EndGroup();
  ImGui::SameLine();
  ImGui::BeginGroup();
  {
    ImGui::SetNextItemWidth(4.5f*em);
    UpdateLevelCombo(&data_->lv);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("output log level");
    }

    {
      gui::ResizeGroup _("##resizer", &size_, {8, fh/em}, {32, 4*fh/em}, em);
      ImGui::InputTextMultiline("##msg", &data_->msg, size_*em);
    }
  }
  ImGui::EndGroup();
}
void LogPrinter::UpdateLevelCombo(notify::Level* lv) noexcept {
  static const char* kNames[] = {"TRAC", "INFO", "WARN", "ERRR"};
  static const int   kCount   = sizeof(kNames)/sizeof(kNames[0]);

  int idx = LevelToInt(*lv);
  if (ImGui::Combo("##level", &idx, kNames, kCount)) {
    *lv = IntToLevel(idx);
  }
}


class MouseInput : public File, public iface::GUI, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<MouseInput>(
      "MouseInput", "retrieves key/mouse data",
      {typeid(iface::Node)});

  MouseInput(const std::shared_ptr<Env>& env) noexcept :
      File(&type_, env), Node(kNone), data_(std::make_shared<UniversalData>()) {
    out_.emplace_back(new OutSock(this, "pos"));
    out_.emplace_back(new OutSock(this, "left"));
    out_.emplace_back(new OutSock(this, "middle"));
    out_.emplace_back(new OutSock(this, "right"));
    out_.emplace_back(new OutSock(this, "gui_active"));

    std::weak_ptr<UniversalData> wdata = data_;
    std::weak_ptr<OutSock>       wp = out_[0];
    std::weak_ptr<OutSock>       wl = out_[1];
    std::weak_ptr<OutSock>       wm = out_[2];
    std::weak_ptr<OutSock>       wr = out_[3];
    std::weak_ptr<OutSock>       wa = out_[4];
    auto task = [wdata, wp, wl, wm, wr, wa](const auto& ctx, auto&&) {
      auto data = wdata.lock();
      if (!data) return;

      auto p = wp.lock();
      if (p) p->Send(ctx, data->pos);

      auto l = wl.lock();
      if (l) l->Send(ctx, data->l);

      auto m = wm.lock();
      if (m) m->Send(ctx, data->m);

      auto r = wr.lock();
      if (r) r->Send(ctx, data->r);

      auto a = wa.lock();
      if (a) a->Send(ctx, data->gui_active);
    };
    in_.emplace_back(new LambdaInSock(this, "clk", std::move(task)));
  }

  static std::unique_ptr<File> Deserialize(
      const msgpack::object&, const std::shared_ptr<Env>& env) {
    return std::make_unique<MouseInput>(env);
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_nil();
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<MouseInput>(env);
  }

  void Update(RefStack&) noexcept override;
  void Update(RefStack&, const std::shared_ptr<Context>&) noexcept override;
  static void UpdateSocket(const char*, float) noexcept;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::GUI, iface::Node>(t).Select(this);
  }

 private:
  struct UniversalData {
    Value::Vec2 pos;
    std::string l, m, r;
    bool gui_active;
  };
  std::shared_ptr<UniversalData> data_;


  static void GetState(std::string& str, ImGuiMouseButton b) noexcept {
    str = "";
    if (ImGui::IsMouseDown(b))          str = "DOWN";
    if (ImGui::IsMouseClicked(b))       str = "CLICK";
    if (ImGui::IsMouseReleased(b))      str = "RELEASE";
    if (ImGui::IsMouseDoubleClicked(b)) str = "DOUBLE";
  }
};
void MouseInput::Update(RefStack&) noexcept {
  const auto p = ImGui::GetMousePos();
  data_->pos = {p.x, p.y};

  GetState(data_->l, ImGuiMouseButton_Left);
  GetState(data_->m, ImGuiMouseButton_Middle);
  GetState(data_->r, ImGuiMouseButton_Right);

  data_->gui_active =
      ImGui::IsAnyItemHovered() ||
      ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
}
void MouseInput::Update(RefStack&, const std::shared_ptr<Context>&) noexcept {
  const auto w = 4*ImGui::GetFontSize();

  ImGui::TextUnformatted("USER INPUT");
  if (ImNodes::BeginInputSlot("clk", 1)) {
    gui::NodeSocket();
    ImGui::SameLine();
    ImGui::TextUnformatted("clk");
    ImNodes::EndSlot();
  }
  ImGui::SameLine();

  ImGui::BeginGroup();
  if (ImNodes::BeginOutputSlot("pos", 1)) {
    UpdateSocket("pos", w);
    ImNodes::EndSlot();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("mouse pos in native window coordinates");
    }
  }
  if (ImNodes::BeginOutputSlot("left", 1)) {
    UpdateSocket("left", w);
    ImNodes::EndSlot();
  }
  if (ImNodes::BeginOutputSlot("middle", 1)) {
    UpdateSocket("middle", w);
    ImNodes::EndSlot();
  }
  if (ImNodes::BeginOutputSlot("right", 1)) {
    UpdateSocket("right", w);
    ImNodes::EndSlot();
  }

  if (ImNodes::BeginOutputSlot("gui_active", 1)) {
    UpdateSocket("GUI active", w);
    ImNodes::EndSlot();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("is any UI component hovered");
    }
  }
  ImGui::EndGroup();
}
void MouseInput::UpdateSocket(const char* s, float w) noexcept {
  const auto tw = ImGui::CalcTextSize(s).x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX()+(w-tw));
  ImGui::TextUnformatted(s);
  ImGui::SameLine();
  gui::NodeSocket();
}

} }  // namespace kingtaker
