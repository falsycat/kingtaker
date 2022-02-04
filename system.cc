#include "kingtaker.hh"

#include <atomic>
#include <thread>

#include <imgui.h>


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

  Time lastModified() const noexcept override { return {}; }
};

} }  // namespace kingtaker
