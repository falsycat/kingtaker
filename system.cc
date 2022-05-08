#include "kingtaker.hh"

#include <cassert>
#include <cinttypes>
#include <map>
#include <memory>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <ImNodes.h>

#include "kingtaker.hh"

#include "iface/dir.hh"
#include "iface/logger.hh"
#include "iface/node.hh"

#include "util/gui.hh"
#include "util/keymap.hh"
#include "util/logger.hh"
#include "util/node.hh"
#include "util/ptr_selector.hh"
#include "util/value.hh"


namespace kingtaker {
namespace {

class GenericDir : public File,
    public iface::Dir,
    public iface::DirItem {
 public:
  static inline TypeInfo kType = TypeInfo::New<GenericDir>(
      "System/GenericDir", "generic impl of directory",
      {typeid(iface::Dir), typeid(iface::DirItem)});

  using ItemList = std::map<std::string, std::unique_ptr<File>>;

  GenericDir(Env*       env,
             ItemList&& items   = {},
             bool       shown   = false) :
      File(&kType, env), DirItem(kTree | kMenu),
      items_(std::move(items)), shown_(shown) {
    for (auto& item : items_) item.second->Move(this, item.first);
  }

  GenericDir(Env* env, const msgpack::object& obj) :
      GenericDir(env, DeserializeItems(env, msgpack::find(obj, "items"s)),
                 msgpack::as_if(msgpack::find(obj, "shown"s), false)) {
  }
  static ItemList DeserializeItems(Env* env, const msgpack::object& obj) {
    if (obj.type != msgpack::type::MAP) throw msgpack::type_error();

    ItemList items;
    for (size_t i = 0; i < obj.via.map.size; ++i) {
      auto& kv = obj.via.map.ptr[i];

      const auto key = kv.key.as<std::string>();
      if (!iface::Dir::ValidateName(key)) {
        throw DeserializeException("invalid name");
      }

      auto [itr, uniq] = items.insert({key, File::Deserialize(env, kv.val)});
      if (!uniq) {
        throw DeserializeException("item name duplication in GenericDir");
      }
      assert(itr->second);
    }
    return items;
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(2);

    pk.pack("shown"s);
    pk.pack(shown_);

    pk.pack("items"s);
    {
      pk.pack_map(static_cast<uint32_t>(items_.size()));
      for (auto& child : items_) {
        pk.pack(child.first);
        child.second->SerializeWithTypeInfo(pk);
      }
    }
  }
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    ItemList items;
    for (auto& p : items_) items[p.first] = p.second->Clone(env);
    return std::make_unique<GenericDir>(env, std::move(items));
  }

  File& Find(std::string_view name) const override {
    const auto sname = std::string(name);

    auto itr = items_.find(sname);
    if (itr == items_.end()) {
      throw NotFoundException("child not found: "+sname);
    }
    return *itr->second;
  }

  File* Add(std::string_view name, std::unique_ptr<File>&& f) noexcept override {
    assert(f);
    auto& items = items_;

    auto [itr, uniq] = items.insert(std::make_pair(std::string(name), std::move(f)));
    if (!uniq) return nullptr;

    Touch();
    itr->second->Move(this, name);
    return itr->second.get();
  }
  std::unique_ptr<File> Remove(std::string_view name) noexcept override {
    auto& items = items_;

    auto itr = items.find(std::string(name));
    if (itr == items.end()) return nullptr;

    auto ret = std::move(itr->second);
    items.erase(itr);

    Touch();
    ret->Move(nullptr, "");
    return ret;
  }

  void Update(Event&) noexcept override;
  void UpdateMenu() noexcept override;
  void UpdateTree() noexcept override;
  void UpdateItem(File* f) noexcept;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Dir, iface::DirItem>(t).
        Select(this);
  }

 private:
  // permanentized params
  ItemList items_;

  bool shown_;

  // volatile params
  std::string name_for_new_;
};
void GenericDir::Update(Event& ev) noexcept {
  for (auto& item : items_) {
    item.second->Update(ev);
  }

  const auto em = ImGui::GetFontSize();
  ImGui::SetNextWindowSize({16*em, 12*em}, ImGuiCond_FirstUseEver);

  if (gui::BeginWindow(this, "TreeView", ev, &shown_)) {
    if (ImGui::BeginPopupContextWindow()) {
      UpdateMenu();
      ImGui::EndPopup();
    }
    UpdateTree();
  }
  gui::EndWindow();
}
void GenericDir::UpdateMenu() noexcept {
  ImGui::PushID(this);

  if (ImGui::BeginMenu("New")) {
    for (const auto& reg : File::registry()) {
      const auto& type = *reg.second;
      if (!type.factory() || !type.IsImplemented<DirItem>()) continue;

      const auto w = 16.f*ImGui::GetFontSize();

      ImGui::SetNextWindowSize({w, 0.f});
      if (ImGui::BeginMenu(reg.first.c_str())) {
        constexpr auto kFlags =
            ImGuiInputTextFlags_EnterReturnsTrue |
            ImGuiInputTextFlags_AutoSelectAll;
        static const char* kHint = "input name and enter";

        ImGui::SetNextItemWidth(w);
        ImGui::SetKeyboardFocusHere();

        const bool enter =
            ImGui::InputTextWithHint("##NameForNew", kHint, &name_for_new_, kFlags);

        const bool dup = items_.end() != items_.find(name_for_new_);
        if (dup) {
          ImGui::Bullet();
          ImGui::Text("name duplication");
        }

        const bool valid = iface::Dir::ValidateName(name_for_new_);
        if (!valid) {
          ImGui::Bullet();
          ImGui::Text("invalid format");
        }

        if (enter && !dup && valid) {
          Queue::main().Push(
              [this, &type]() { Add(name_for_new_, type.Create(&env())); });
        }
        ImGui::EndMenu();
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", type.desc().c_str());
      }
    }
    ImGui::EndMenu();
  }

  ImGui::Separator();
  ImGui::MenuItem("TreeView", nullptr, &shown_);

  ImGui::PopID();
}
void GenericDir::UpdateTree() noexcept {
  for (auto& child : items_) {
    UpdateItem(child.second.get());
  }
}
void GenericDir::UpdateItem(File* f) noexcept {
  ImGuiTreeNodeFlags flags =
      ImGuiTreeNodeFlags_NoTreePushOnOpen |
      ImGuiTreeNodeFlags_SpanFullWidth;

  auto* ditem = File::iface<iface::DirItem>(f);
  if (ditem && !(ditem->flags() & kTree)) {
    flags |= ImGuiTreeNodeFlags_Leaf;
  }

  const bool open = ImGui::TreeNodeEx(f, flags, "%s", f->name().c_str());
  if (ImGui::IsItemHovered()) {
    ImGui::BeginTooltip();
    ImGui::Text("%s", f->type().name().c_str());
    ImGui::Text("%s", f->abspath().Stringify().c_str());
    if (ditem && (ditem->flags() & kTooltip)) {
      ditem->UpdateTooltip();
    }
    ImGui::EndTooltip();
  }
  if (ImGui::BeginPopupContextItem()) {
    if (ImGui::MenuItem("Remove")) {
      const std::string name = f->name();
      Queue::main().Push([this, name]() { Remove(name); });
    }
    if (ImGui::MenuItem("Rename")) {
      Queue::main().Push([]() { throw Exception("not implemented"); });
    }
    if (ditem && (ditem->flags() & kMenu)) {
      ImGui::Separator();
      ditem->UpdateMenu();
    }
    ImGui::EndPopup();
  }
  if (open) {
    ImGui::TreePush(f);
    if (ditem && (ditem->flags() & kTree)) {
      ditem->UpdateTree();
    }
    ImGui::TreePop();
  }
}


class ImGuiConfig : public File {
 public:
  static inline TypeInfo kType = TypeInfo::New<ImGuiConfig>(
      "System/ImGuiConfig", "saves and restores ImGui config", {});

  ImGuiConfig(Env* env, const std::string& v = "") noexcept :
      File(&kType, env) {
    ImGui::LoadIniSettingsFromMemory(v.data(), v.size());
  }

  ImGuiConfig(Env* env, const msgpack::object& obj) :
      ImGuiConfig(env, obj.as<std::string>()) {
  }
  void Serialize(Packer& pk) const noexcept override {
    size_t n;
    const char* ini = ImGui::SaveIniSettingsToMemory(&n);
    pk.pack_str(static_cast<uint32_t>(n));
    pk.pack_str_body(ini, static_cast<uint32_t>(n));
  }
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    return std::make_unique<ImGuiConfig>(env);
  }
};


class ClockPulseGenerator final : public File, public iface::DirItem {
 public:
  static inline TypeInfo kType = TypeInfo::New<ClockPulseGenerator>(
      "System/ClockPulseGenerator", "emits a pulse into a specific node on each GUI updates",
      {typeid(iface::DirItem)});

  ClockPulseGenerator(Env*               env,
                      const std::string& path      = "",
                      const std::string& sock_name = "",
                      bool               shown     = false,
                      bool               enable    = false) noexcept :
      File(&kType, env), DirItem(kNone),
      path_(path), sock_name_(sock_name), shown_(shown), enable_(enable),
      logq_(std::make_shared<LoggerTemporaryItemQueue>()) {
  }

  ClockPulseGenerator(Env* env, const msgpack::object& obj) :
      ClockPulseGenerator(env,
                          msgpack::find(obj, "path"s).as<std::string>(),
                          msgpack::find(obj, "sock_name"s).as<std::string>(),
                          msgpack::as_if<bool>(msgpack::find(obj, "shown"s), false),
                          msgpack::as_if<bool>(msgpack::find(obj, "enable"s), false)) {
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
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    return std::make_unique<ClockPulseGenerator>(env, path_, sock_name_, shown_, enable_);
  }

  void Update(Event& ev) noexcept override {
    if (enable_) Emit();

    if (gui::BeginWindow(this, "ClockPulseGenerator", ev, &shown_)) {
      UpdateEditor();
    }
    gui::EndWindow();
    logq_->Flush(*this);
  }
  void UpdateEditor() noexcept;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::DirItem>(t).Select(this);
  }

 private:
  // permanentized params
  std::string path_;
  std::string sock_name_;

  bool shown_;
  bool enable_;

  // volatile params
  std::shared_ptr<LoggerTemporaryItemQueue> logq_;

  std::string path_editing_;


  void Emit() noexcept {
    try {
      auto& target = Resolve(path_);

      auto n = File::iface<iface::Node>(&target);
      if (!n) throw Exception("target doesn't have Node interface");

      auto sock = n->in(sock_name_);
      if (!sock) throw Exception("missing input socket, "+sock_name_);

      auto ctx = std::make_shared<InnerContext>(this, target.abspath());
      n->Initialize(ctx);
      sock->Receive(ctx, Value::Pulse());
    } catch (Exception& e) {
      logq_->Push(LoggerTextItem::Error(abspath(), e.msg()));
      enable_ = false;
    }
  }


  class InnerContext final : public iface::Node::Context {
   public:
    InnerContext(ClockPulseGenerator* owner, File::Path&& path) noexcept :
        Context(std::move(path)), logq_(owner->logq_) {
    }
    void Notify(const std::shared_ptr<iface::Logger::Item>& item) noexcept override {
      logq_->Push(item);
    }
   private:
    std::shared_ptr<LoggerTemporaryItemQueue> logq_;
  };
};
void ClockPulseGenerator::UpdateEditor() noexcept {
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
        if (gui::InputPathMenu("##path_input", this, &path_editing_)) {
          path_ = std::move(path_editing_);
        }
        ImGui::EndPopup();
      }
    }
    if (enable_) ImGui::EndDisabled();

    // pulse emit button
    ImGui::SameLine();
    if (ImGui::Button("Z")) {
      Emit();
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


class Logger final : public File, public iface::DirItem, public iface::Logger {
 public:
  static inline TypeInfo kType = TypeInfo::New<Logger>(
      "System/Logger", "",
      {typeid(iface::DirItem), typeid(iface::Logger)});

  Logger(Env* env, bool shown = true) noexcept :
      File(&kType, env), DirItem(DirItem::kNone), shown_(shown) {
  }

  Logger(Env* env, const msgpack::object& obj) noexcept :
      Logger(env, msgpack::as_if<bool>(obj, false)) {
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack(shown_);
  }
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    return std::make_unique<Logger>(env, shown_);
  }

  void Push(std::shared_ptr<Logger::Item>&& item) noexcept override {
    items_.push_back(std::move(item));
  }

  void Update(Event&) noexcept override;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::DirItem, iface::Logger>(t).Select(this);
  }

 private:
  bool shown_;

  std::deque<std::shared_ptr<Logger::Item>> items_;
};
void Logger::Update(Event& ev) noexcept {
  if (gui::BeginWindow(this, "Logger", ev, &shown_)) {
    constexpr auto kTableFlags =
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_Hideable  |
        ImGuiTableFlags_RowBg     |
        ImGuiTableFlags_Borders   |
        ImGuiTableFlags_ContextMenuInBody |
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("list", 3, kTableFlags, ImGui::GetContentRegionAvail(), 0)) {
      ImGui::TableSetupColumn("level");
      ImGui::TableSetupColumn("summary");
      ImGui::TableSetupColumn("location");
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableHeadersRow();

      for (const auto& item : items_) {
        ImGui::TableNextRow();
        ImGui::PushID(item.get());

        if (ImGui::TableSetColumnIndex(0)) {
          constexpr auto kFlags =
              ImGuiSelectableFlags_SpanAllColumns |
              ImGuiSelectableFlags_AllowItemOverlap;
          const char* str =
              item->lv() == kInfo?  "INFO":
              item->lv() == kWarn?  "WARN":
              item->lv() == kError? "ERRR": "XD";
          ImGui::Selectable(str, false, kFlags);
          if (ImGui::BeginPopupContextItem()) {
            item->UpdateMenu();
            ImGui::EndPopup();
          }
        }
        if (ImGui::TableNextColumn()) {
          ImGui::BeginGroup();
          item->UpdateSummary();
          ImGui::EndGroup();
          if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            item->UpdateTooltip();
            ImGui::EndTooltip();
          }
        }
        if (ImGui::TableNextColumn()) {
          const auto& srcloc = item->srcloc();

          const auto file = srcloc.file_name();
          const auto line = static_cast<uintmax_t>(srcloc.line());
          ImGui::Text("%s:%" PRIuMAX, file, line);
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s:%" PRIuMAX, file, line);
          }
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
  }
  gui::EndWindow();
}

} }  // namespace kingtaker
