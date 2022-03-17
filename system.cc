#include "kingtaker.hh"

#include <atomic>
#include <thread>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "iface/gui.hh"

#include "util/notify.hh"
#include "util/ptr_selector.hh"


namespace kingtaker {
namespace {

class SystemImGuiConfig : public File {
 public:
  static inline TypeInfo type_ = TypeInfo::New<SystemImGuiConfig>(
      "SystemImGuiConfig", "save/restore ImGui config", {});

  SystemImGuiConfig() : File(&type_) { }

  std::unique_ptr<File> Clone() const noexcept override {
    return std::make_unique<SystemImGuiConfig>();
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj) {
    if (obj.type == msgpack::type::STR) {
      const auto& str = obj.via.str;
      ImGui::LoadIniSettingsFromMemory(str.ptr, str.size);
    }
    return std::make_unique<SystemImGuiConfig>();
  }
  void Serialize(Packer& pk) const noexcept override {
    size_t n;
    const char* ini = ImGui::SaveIniSettingsToMemory(&n);
    pk.pack_str(static_cast<uint32_t>(n));
    pk.pack_str_body(ini, static_cast<uint32_t>(n));
  }
};

class SystemLogger : public File, public iface::GUI {
 public:
  static inline TypeInfo type_ = TypeInfo::New<SystemLogger>(
      "SystemLogger", "logger window", {});

  static inline const auto kIdSuffix = ": SystemLogger";

  SystemLogger(bool shown = true) : File(&type_), shown_(shown) { }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj) {
    return std::make_unique<SystemLogger>(obj.as<bool>());
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack(shown_);
  }
  std::unique_ptr<File> Clone() const noexcept override {
    return std::make_unique<SystemLogger>(shown_);
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

} }  // namespace kingtaker
