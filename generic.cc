#include <cassert>
#include <map>
#include <memory>
#include <string>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <msgpack.hh>

#include "kingtaker.hh"

#include "iface/dir.hh"
#include "iface/gui.hh"

#include "util/ptr_selector.hh"


namespace kingtaker {
namespace {

// A simple implementation of directory file.
class GenericDir : public File,
    public iface::Dir,
    public iface::DirItem,
    public iface::GUI {
 public:
  static inline TypeInfo type_ = TypeInfo::New<GenericDir>(
      "GenericDir", "generic impl of directory",
      {typeid(iface::Dir), typeid(iface::DirItem), typeid(iface::GUI)});

  using ItemList = std::map<std::string, std::unique_ptr<File>>;

  GenericDir(const std::shared_ptr<Env>& env,
             ItemList&& items   = {},
             Time       lastmod = Clock::now(),
             bool       shown   = false) :
      File(&type_, env), DirItem(kTree | kMenu),
      items_(std::move(items)), lastmod_(lastmod), shown_(shown) {
  }

  static std::unique_ptr<GenericDir> Deserialize(const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    try {
      ItemList items;

      auto& obj_items = msgpack::find(obj, "items"s);
      if (obj_items.type != msgpack::type::MAP) throw msgpack::type_error();

      for (size_t i = 0; i < obj_items.via.map.size; ++i) {
        auto& kv = obj_items.via.map.ptr[i];

        const auto key = kv.key.as<std::string>();
        if (!iface::Dir::ValidateName(key)) {
          throw DeserializeException("invalid name");
        }

        auto [itr, uniq] = items.insert({key, File::Deserialize(kv.val, env)});
        if (!uniq) {
          throw DeserializeException("item name duplication in GenericDir");
        }
        assert(itr->second);
      }
      return std::make_unique<GenericDir>(
          env,
          std::move(items),
          msgpack::as_if(msgpack::find(obj, "lastmod"s), Clock::now()),
          msgpack::as_if(msgpack::find(obj, "shown"s), false));
    } catch (msgpack::type_error& e) {
      throw DeserializeException("broken GenericDir: "s+e.what());
    }
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(3);

    pk.pack("lastmod"s);
    pk.pack(lastmod_);

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
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    ItemList items;
    for (auto& p : items_) items[p.first] = p.second->Clone(env);
    return std::make_unique<GenericDir>(env, std::move(items));
  }

  File* Find(std::string_view name) const noexcept override {
    auto itr = items_.find(std::string(name));
    if (itr == items_.end()) return nullptr;
    return itr->second.get();
  }
  void Scan(std::function<void(std::string_view, File*)> L) const noexcept override {
    for (auto& p : items_) L(p.first, p.second.get());
  }

  File* Add(std::string_view name, std::unique_ptr<File>&& f) noexcept override {
    assert(f);
    auto& items = items_;

    auto [itr, uniq] = items.insert(std::make_pair(std::string(name), std::move(f)));
    if (!uniq) return nullptr;

    lastmod_ = Clock::now();
    return itr->second.get();
  }
  std::unique_ptr<File> Remove(std::string_view name) noexcept override {
    auto& items = items_;

    auto itr = items.find(std::string(name));
    if (itr == items.end()) return nullptr;

    auto ret = std::move(itr->second);
    items.erase(itr);

    lastmod_ = Clock::now();
    return ret;
  }

  void OnSaved(File::RefStack& ref) noexcept override {
    Iterate(ref, [](auto& ref, auto& gui) { gui.OnSaved(ref); });
  }
  bool OnClosing(File::RefStack& ref) noexcept override {
    bool closing = true;
    Iterate(ref, [&closing](auto& ref, auto& gui) {
              closing = gui.OnClosing(ref) && closing;
            });
    return closing;
  }
  void OnClosed(File::RefStack& ref) noexcept override {
    Iterate(ref, [](auto& ref, auto& gui) { gui.OnClosed(ref); });
  }

  void Update(RefStack& ref) noexcept override;
  void UpdateMenu(RefStack&) noexcept override;
  void UpdateTree(RefStack& ref) noexcept override;
  void UpdateItem(RefStack& ref, File* f) noexcept;

  Time lastModified() const noexcept override {
    return lastmod_;
  }
  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Dir, iface::DirItem, iface::GUI>(t).
        Select(this);
  }

 private:
  // permanentized params
  ItemList items_;

  Time lastmod_;

  bool shown_;

  // volatile params
  std::string name_for_new_;


  void Iterate(File::RefStack& ref,
               const std::function<void(File::RefStack&, iface::GUI&)>& f) {
    for (auto& child : items_) {
      ref.Push({child.first, child.second.get()});
      f(ref, File::iface(*child.second, iface::GUI::null()));
      ref.Pop();
    }
  }
};
void GenericDir::Update(File::RefStack& ref) noexcept {
  Iterate(ref, [](auto& ref, auto& gui) { gui.Update(ref); });
  if (!shown_) return;

  const auto em = ImGui::GetFontSize();
  ImGui::SetNextWindowSize({16*em, 12*em}, ImGuiCond_FirstUseEver);

  const auto id = ref.Stringify()+": GenericDir TreeView";
  if (ImGui::Begin(id.c_str(), &shown_)) {
    if (ImGui::BeginPopupContextWindow()) {
      UpdateMenu(ref);
      ImGui::EndPopup();
    }
    UpdateTree(ref);
  }
  ImGui::End();
}
void GenericDir::UpdateMenu(File::RefStack&) noexcept {
  ImGui::PushID(this);

  if (ImGui::BeginMenu("New")) {
    for (const auto& reg : File::registry()) {
      const auto& type = *reg.second;
      if (!type.factory() || !type.CheckImplemented<DirItem>()) continue;

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

        const bool dup = !!Find(name_for_new_);
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
              [this, &type]() { Add(name_for_new_, type.Create(env())); });
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
void GenericDir::UpdateTree(File::RefStack& ref) noexcept {
  for (auto& child : items_) {
    ref.Push({child.first, child.second.get()});
    UpdateItem(ref, child.second.get());
    ref.Pop();
  }
}
void GenericDir::UpdateItem(File::RefStack& ref, File* f) noexcept {
  ImGuiTreeNodeFlags flags =
      ImGuiTreeNodeFlags_NoTreePushOnOpen |
      ImGuiTreeNodeFlags_SpanFullWidth;

  auto* ditem = File::iface<iface::DirItem>(f);
  if (ditem && !(ditem->flags() & kTree)) {
    flags |= ImGuiTreeNodeFlags_Leaf;
  }

  const bool open = ImGui::TreeNodeEx(f, flags, "%s", ref.top().name().c_str());
  if (ImGui::IsItemHovered()) {
    ImGui::BeginTooltip();
    ImGui::Text("%s", f->type().name().c_str());
    ImGui::Text("%s", ref.Stringify().c_str());
    if (ditem && (ditem->flags() & kTooltip)) {
      ditem->UpdateTooltip(ref);
    }
    ImGui::EndTooltip();
  }
  if (ImGui::BeginPopupContextItem()) {
    if (ImGui::MenuItem("Remove")) {
      const std::string name = ref.top().name();
      Queue::main().Push([this, name]() { Remove(name); });
    }
    if (ImGui::MenuItem("Rename")) {
      Queue::main().Push([]() { throw Exception("not implemented"); });
    }
    if (ditem && (ditem->flags() & kMenu)) {
      ImGui::Separator();
      ditem->UpdateMenu(ref);
    }
    ImGui::EndPopup();
  }
  if (open) {
    ImGui::TreePush(f);
    if (ditem && (ditem->flags() & kTree)) {
      ditem->UpdateTree(ref);
    }
    ImGui::TreePop();
  }
}

} }  // namespace kingtaker
