#include "kingtaker.hh"

#include <atomic>
#include <thread>

#include <imgui.h>
#include <imgui_stdlib.h>

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


class PulseGenerator : public File, public iface::DirItem, public iface::GUI {
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

} }  // namespace kingtaker
