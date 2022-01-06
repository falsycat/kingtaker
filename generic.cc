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


namespace kingtaker {
namespace {

class GenericDir : public File {
 public:
  static inline TypeInfo type_ = TypeInfo::New<GenericDir>(
      "GenericDir", "generic impl of directory",
      {typeid(iface::Dir), typeid(iface::DirItem), typeid(iface::GUI)});

  using ItemList = std::map<std::string, std::unique_ptr<File>>;

  GenericDir(ItemList&& items = {}) :
      File(&type_),
      last_modified_(Clock::now()), items_(std::move(items)),
      dir_(this), gui_(this) {
  }

  File* Find(std::string_view name) const noexcept override {
    auto itr = items_.find(std::string(name));
    if (itr == items_.end()) return nullptr;
    return itr->second.get();
  }
  void Scan(std::function<void(std::string_view, File*)> L) const noexcept {
    for (auto& p : items_) L(p.first, p.second.get());
  }

  std::unique_ptr<File> Clone() const noexcept override {
    ItemList items;
    for (auto& p : items_) items[p.first] = p.second->Clone();
    return std::make_unique<GenericDir>(std::move(items));
  }

  static std::unique_ptr<GenericDir> Deserialize(const msgpack::object& obj) {
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

        auto [itr, uniq] = items.insert({key, File::Deserialize(kv.val)});
        if (!uniq) {
          throw DeserializeException("item name duplication in GenericDir");
        }
        assert(itr->second);
      }
      auto ret = std::make_unique<GenericDir>(std::move(items));
      ret->gui_.Deserialize(msgpack::find(obj, "gui"s));
      return ret;

    } catch (msgpack::type_error& e) {
      throw DeserializeException("GenericDir broken: "s+e.what());
    }
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(2);

    pk.pack("gui"s);
    gui_.Serialize(pk);

    pk.pack("items"s);
    {
      pk.pack_map(static_cast<uint32_t>(items_.size()));
      for (auto& child : items_) {
        pk.pack(child.first);
        child.second->SerializeWithTypeInfo(pk);
      }
    }
  }

  Time lastModified() const noexcept override {
    return last_modified_;
  }
  void* iface(const std::type_index& t) noexcept override {
    return
        t == typeid(iface::Dir)?     static_cast<void*>(&dir_):
        t == typeid(iface::DirItem)? static_cast<void*>(&gui_):
        t == typeid(iface::GUI)?     static_cast<void*>(&gui_):
        nullptr;
  }

 private:
  Time last_modified_;

  ItemList items_;

  class Dir final : public iface::Dir {
   public:
    Dir(GenericDir* owner) : owner_(owner) { }

    File* Add(std::string_view name, std::unique_ptr<File>&& f) noexcept override {
      assert(f);
      auto& items = owner_->items_;

      auto [itr, uniq] = items.insert(std::make_pair(std::string(name), std::move(f)));
      if (!uniq) return nullptr;

      owner_->last_modified_ = Clock::now();
      return itr->second.get();
    }
    std::unique_ptr<File> Remove(std::string_view name) noexcept override {
      auto& items = owner_->items_;

      auto itr = items.find(std::string(name));
      if (itr == items.end()) return nullptr;

      auto ret = std::move(itr->second);
      items.erase(itr);

      owner_->last_modified_ = Clock::now();
      return ret;
    }

   private:
    GenericDir* owner_;
  } dir_;

  class GUI final : public iface::GUI, public iface::DirItem {
   public:
    GUI(GenericDir* owner) : DirItem(kTree | kMenu), owner_(owner) { }

    void Deserialize(const msgpack::object& obj) noexcept {
      try {
        shown_ = msgpack::find(obj, "shown"s).as<bool>();
      } catch (msgpack::type_error&) {
        shown_ = false;
      }
    }
    void Serialize(Packer& pk) const noexcept {
      pk.pack_map(1);
      pk.pack("shown"); pk.pack(shown_);
    }

    void Update(File::RefStack& ref) noexcept override {
      for (auto& child : owner_->items_) {
        ref.Push({child.first, child.second.get()});
        File::iface(*child.second, iface::GUI::null()).Update(ref);
        ref.Pop();
      }

      if (!shown_) return;

      const auto em = ImGui::GetFontSize();
      ImGui::SetNextWindowSize({16*em, 12*em}, ImGuiCond_FirstUseEver);

      const auto id = ref.Stringify()+": GenericDir TreeView";
      if (ImGui::Begin(id.c_str())) {
        if (ImGui::BeginPopupContextWindow()) {
          UpdateMenu(ref);
          ImGui::EndPopup();
        }
        UpdateTree(ref);
      }
      ImGui::End();
    }
    void UpdateMenu(File::RefStack&) noexcept override {
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

            const bool dup = !!owner_->Find(name_for_new_);
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
              File::QueueMainTask(
                  [&dir = owner_->dir_, name = name_for_new_, &type]() {
                    dir.Add(name, type.Create());
                  });
            }
            ImGui::EndMenu();
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(type.desc().c_str());
          }
        }

        ImGui::EndMenu();
      }

      ImGui::Separator();
      ImGui::MenuItem("TreeView", nullptr, &shown_);

      ImGui::PopID();
    }
    void UpdateTree(File::RefStack& ref) noexcept override {
      for (auto& child : owner_->items_) {
        ref.Push({child.first, child.second.get()});
        UpdateItem(ref, child.second.get());
        ref.Pop();
      }
    }

   private:
    void UpdateItem(RefStack& ref, File* f) noexcept {
      ImGuiTreeNodeFlags flags =
          ImGuiTreeNodeFlags_NoTreePushOnOpen |
          ImGuiTreeNodeFlags_SpanFullWidth;

      auto* ditem = File::iface<iface::DirItem>(f);
      if (ditem && !(ditem->flags() & kTree)) {
        flags |= ImGuiTreeNodeFlags_Leaf;
      }

      const bool open = ImGui::TreeNodeEx(f, flags, ref.top().name().c_str());
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
          File::QueueMainTask([this, name]() { owner_->dir_.Remove(name); });
        }
        if (ImGui::MenuItem("Rename")) {
          File::QueueMainTask([this]() { throw Exception("not implemented"); });
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

    GenericDir* owner_;

    std::string name_for_new_;

    // values to be serialized
    bool shown_ = false;
  } gui_;
};

} }  // namespace kingtaker
