#include "kingtaker.hh"

#include <cassert>
#include <map>
#include <memory>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <ImNodes.h>

#include "kingtaker.hh"

#include "iface/dir.hh"
#include "iface/gui.hh"
#include "iface/node.hh"

#include "util/gui.hh"
#include "util/keymap.hh"
#include "util/notify.hh"
#include "util/ptr_selector.hh"
#include "util/value.hh"


namespace kingtaker {
namespace {

// A simple implementation of directory file.
class GenericDir : public File,
    public iface::Dir,
    public iface::DirItem,
    public iface::GUI {
 public:
  static inline TypeInfo type_ = TypeInfo::New<GenericDir>(
      "System/GenericDir", "generic impl of directory",
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


// Saves and restores permanentized parameters of ImGui.
class ImGuiConfig : public File {
 public:
  static inline TypeInfo type_ = TypeInfo::New<ImGuiConfig>(
      "System/ImGuiConfig", "saves and restores ImGui config", {});

  ImGuiConfig(const std::shared_ptr<Env>& env) noexcept :
      File(&type_, env) {
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    if (obj.type == msgpack::type::STR) {
      const auto& str = obj.via.str;
      ImGui::LoadIniSettingsFromMemory(str.ptr, str.size);
    }
    return std::make_unique<ImGuiConfig>(env);
  }
  void Serialize(Packer& pk) const noexcept override {
    size_t n;
    const char* ini = ImGui::SaveIniSettingsToMemory(&n);
    pk.pack_str(static_cast<uint32_t>(n));
    pk.pack_str_body(ini, static_cast<uint32_t>(n));
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<ImGuiConfig>(env);
  }
};


class LogView : public File, public iface::GUI {
 public:
  static inline TypeInfo type_ = TypeInfo::New<LogView>(
      "System/LogView", "provides system log viewer", {});

  static inline const auto kIdSuffix = ": LogView";

  LogView(const std::shared_ptr<Env>& env, bool shown = true) noexcept :
      File(&type_, env), shown_(shown) {
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    return std::make_unique<LogView>(env, obj.as<bool>());
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack(shown_);
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<LogView>(env, shown_);
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


class ClockPulseGenerator final : public File, public iface::DirItem, public iface::GUI {
 public:
  static inline TypeInfo type_ = TypeInfo::New<ClockPulseGenerator>(
      "System/ClockPulseGenerator", "emits a pulse into a specific node on each GUI updates",
      {typeid(iface::DirItem), typeid(iface::GUI)});

  static inline const auto kIdSuffix = ": ClockPulseGenerator";

  ClockPulseGenerator(const std::shared_ptr<Env>& env,
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
      return std::make_unique<ClockPulseGenerator>(
          env,
          msgpack::find(obj, "path"s).as<std::string>(),
          msgpack::find(obj, "sock_name"s).as<std::string>(),
          msgpack::as_if<bool>(msgpack::find(obj, "shown"s), false),
          msgpack::as_if<bool>(msgpack::find(obj, "enable"s), false));
    } catch (msgpack::type_error&) {
      throw DeserializeException("broken ClockPulseGenerator");
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
    return std::make_unique<ClockPulseGenerator>(env, path_, sock_name_, shown_, enable_);
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
void ClockPulseGenerator::UpdateEditor(RefStack& ref) noexcept {
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


class Logger final : public File, public iface::Node, public iface::GUI {
 public:
  static inline TypeInfo type_ = TypeInfo::New<Logger>(
      "System/Logger", "prints msg to system log",
      {typeid(iface::Node), typeid(iface::GUI)});

  Logger(const std::shared_ptr<Env>& env,
         notify::Level      lv   = notify::kTrace,
         const std::string& msg  = "",
         ImVec2             size = {0, 0}) noexcept :
      File(&type_, env), Node(kNone),
      data_(std::make_shared<UniversalData>(lv, msg)), size_(size) {
    std::weak_ptr<UniversalData> wdata = data_;
    auto task = [self = this, wdata](const auto&, auto&& v) {
      auto data = wdata.lock();
      if (!data) return;
      auto msg = data->msg;

      const auto d = msg.find('$');
      if (d != std::string::npos) {
        msg = msg.substr(0, d)+v.StringifyType()+msg.substr(d+1);
      }

      const auto p = msg.find('%');
      if (p != std::string::npos) {
        msg = msg.substr(0, p)+v.Stringify()+msg.substr(p+1);
      }

      notify::Push(
          {std::source_location::current(), data->lv, msg, File::Path(data->path), self});
    };
    in_.emplace_back(new LambdaInSock(this, "CLK", std::move(task)));
  }

  static std::unique_ptr<File> Deserialize(
      const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    try {
      const auto size = msgpack::as_if<std::tuple<float, float>>(
          msgpack::find(obj, "size"s), {0.f, 0.f});

      return std::make_unique<Logger>(
          env,
          IntToLevel(msgpack::as_if<int>(msgpack::find(obj, "level"s), 0)),
          msgpack::as_if<std::string>(msgpack::find(obj, "msg"s), ""s),
          ImVec2 {std::get<0>(size), std::get<1>(size)});
    } catch (msgpack::type_error&) {
      return std::make_unique<Logger>(env);
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
    return std::make_unique<Logger>(env, data_->lv, data_->msg, size_);
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
void Logger::Update(RefStack& ref) noexcept {
  data_->path = ref.GetFullPath();
}
void Logger::Update(RefStack&, const std::shared_ptr<Context>&) noexcept {
  const auto em = ImGui::GetFontSize();
  const auto fh = ImGui::GetFrameHeight();

  ImGui::TextUnformatted("SYSTEM LOGGER");

  ImGui::BeginGroup();
  {
    if (ImNodes::BeginInputSlot("CLK", 1)) {
      ImGui::AlignTextToFramePadding();
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
    ImGui::SameLine();
    {
      gui::ResizeGroup _("##resizer", &size_, {8, fh/em}, {32, 4*fh/em}, em);
      ImGui::InputTextMultiline("##msg", &data_->msg, size_*em);
    }
  }
  ImGui::EndGroup();
}
void Logger::UpdateLevelCombo(notify::Level* lv) noexcept {
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
      "System/MouseInput", "retrieves mouse state",
      {typeid(iface::GUI), typeid(iface::Node)});

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
    in_.emplace_back(new LambdaInSock(this, "CLK", std::move(task)));
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
void MouseInput::Update(RefStack&, const std::shared_ptr<Context>& ctx) noexcept {
  const auto w = 4*ImGui::GetFontSize();

  ImGui::TextUnformatted("MOUSE INPUT");
  if (ImNodes::BeginInputSlot("CLK", 1)) {
    gui::NodeSocket();
    ImGui::SameLine();
    if (ImGui::SmallButton("CLK")) {
      Queue::sub().Push([clk = in_[0], ctx]() { clk->Receive(ctx, {}); });
    }
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


class KeyInput final : public File, public iface::GUI, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<KeyInput>(
      "System/KeyInput", "retrieves key state",
      {typeid(iface::GUI), typeid(iface::Node)});

  KeyInput(const std::shared_ptr<Env>& env, const std::string& key = "(none)") noexcept :
      File(&type_, env), Node(Node::kNone),
      data_(std::make_shared<UniversalData>()), key_(key) {
    out_.emplace_back(new OutSock(this, "down"));
    out_.emplace_back(new OutSock(this, "press"));
    out_.emplace_back(new OutSock(this, "repeat"));
    out_.emplace_back(new OutSock(this, "release"));

    std::weak_ptr<UniversalData> wdata = data_;
    std::weak_ptr<OutSock>       wd    = out_[0];
    std::weak_ptr<OutSock>       wp    = out_[1];
    std::weak_ptr<OutSock>       wrp   = out_[2];
    std::weak_ptr<OutSock>       wr    = out_[3];
    auto task = [wdata, wd, wp, wrp, wr](const auto& ctx, auto&&) {
      auto data = wdata.lock();
      if (!data) return;
      const auto state = data->state;

      auto d = wd.lock();
      if (d && state & State::kDown) d->Send(ctx, {});

      auto p = wp.lock();
      if (p && state & State::kPress) p->Send(ctx, {});

      auto rp = wrp.lock();
      if (rp && state & State::kRepeat) rp->Send(ctx, {});

      auto r = wr.lock();
      if (r && state & State::kRelease) r->Send(ctx, {});
    };
    in_.emplace_back(new LambdaInSock(this, "CLK", std::move(task)));
  }

  static std::unique_ptr<File> Deserialize(
      const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    try {
      return std::make_unique<KeyInput>(
          env, msgpack::as_if<std::string>(obj, "(none)"s));
    } catch (msgpack::type_error&) {
      throw DeserializeException("broken System/KeyInput");
    }
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack(key_);
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<KeyInput>(env, key_);
  }

  void Update(RefStack&) noexcept override {
    FetchKeyCode();

    auto& st = data_->state;
    st = State::kNone;

    if (keycode_ && !ImGui::IsAnyItemActive()) {
      if (ImGui::IsKeyDown(*keycode_)) {
        st = st | kDown;
      }
      if (ImGui::IsKeyPressed(*keycode_, false)) {
        st = st | kPress;
      } else if (ImGui::IsKeyPressed(*keycode_, true)) {
        st = st | kRepeat;
      }
      if (ImGui::IsKeyReleased(*keycode_)) {
        st = st | kRelease;
      }
    }
  }
  void Update(RefStack&, const std::shared_ptr<Context>&) noexcept override;
  static void UpdateSocket(const char*, float) noexcept;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::GUI, iface::Node>(t).Select(this);
  }

 private:
  enum State : uint8_t  {
    kNone    = 0,
    kDown    = 1 << 0,
    kPress   = 1 << 1,
    kRepeat  = 1 << 2,
    kRelease = 1 << 3,
  };
  struct UniversalData final {
    uint8_t state = kNone;
  };
  std::shared_ptr<UniversalData> data_;

  // permanentized params
  std::string key_;

  // volatile params
  std::optional<ImGuiKey> keycode_ = std::nullopt;


  void FetchKeyCode() noexcept {
    if (keycode_) return;
    auto itr = kKeyMap.find(key_);
    if (itr != kKeyMap.end()) {
      keycode_ = itr->second;
    }
  }
};
void KeyInput::Update(RefStack&, const std::shared_ptr<Context>& ctx) noexcept {
  const auto em = ImGui::GetFontSize();
  const auto w  = 4*em;

  ImGui::TextUnformatted("KEY INPUT:");
  ImGui::SameLine();
  ImGui::SmallButton(key_.c_str());

  gui::NodeCanvasResetZoom();
  if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
    FetchKeyCode();
    for (auto& p : kKeyMap) {
      const bool sel = (keycode_ && *keycode_ == p.second);
      if (ImGui::MenuItem(p.first.c_str(), nullptr, sel)) {
        key_     = p.first;
        keycode_ = p.second;
      }
    }
    ImGui::EndPopup();
  }
  gui::NodeCanvasSetZoom();

  if (ImNodes::BeginInputSlot("CLK", 1)) {
    gui::NodeSocket();
    ImGui::SameLine();
    if (ImGui::SmallButton("CLK")) {
      Queue::sub().Push([clk = in_[0], ctx]() { clk->Receive(ctx, {}); });
    }
    ImNodes::EndSlot();
  }
  ImGui::SameLine();

  ImGui::BeginGroup();
  if (ImNodes::BeginOutputSlot("down", 1)) {
    UpdateSocket("down", w);
    ImNodes::EndSlot();
  }
  if (ImNodes::BeginOutputSlot("press", 1)) {
    UpdateSocket("press", w);
    ImNodes::EndSlot();
  }
  if (ImNodes::BeginOutputSlot("repeat", 1)) {
    UpdateSocket("repeat", w);
    ImNodes::EndSlot();
  }
  if (ImNodes::BeginOutputSlot("release", 1)) {
    UpdateSocket("release", w);
    ImNodes::EndSlot();
  }
  ImGui::EndGroup();
}
void KeyInput::UpdateSocket(const char* s, float w) noexcept {
  const auto tw = ImGui::CalcTextSize(s).x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX()+(w-tw));
  ImGui::TextUnformatted(s);
  ImGui::SameLine();
  gui::NodeSocket();
}

} }  // namespace kingtaker
