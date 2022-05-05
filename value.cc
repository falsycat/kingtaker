#include "kingtaker.hh"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <fstream>
#include <memory>
#include <optional>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <ImNodes.h>

#include "iface/dir.hh"
#include "iface/memento.hh"
#include "iface/node.hh"

#include "util/format.hh"
#include "util/gui.hh"
#include "util/memento.hh"
#include "util/node.hh"
#include "util/ptr_selector.hh"
#include "util/value.hh"


namespace kingtaker {
namespace {

class Imm final : public File, public iface::DirItem, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<Imm>(
      "Value/Imm", "immediate value",
      {typeid(iface::Memento), typeid(iface::DirItem), typeid(iface::Node)});

  Imm(Env* env, Value&& v = Value::Integer {0}, ImVec2 size = {0, 0}) noexcept :
      File(&type_, env), DirItem(DirItem::kTree), Node(Node::kNone),
      mem_({this, std::move(v), size}),
      sock_out_(this, "out"),
      sock_clk_(this, "clk",
                [this](auto& ctx, auto&&) {
                  sock_out_.Send(ctx, Value(mem_.data().value));
                }) {
    in_  = {&sock_clk_};
    out_ = {&sock_out_};
  }

  Imm(Env* env, const msgpack::object& obj) :
      Imm(env,
          Value(msgpack::find(obj, "value"s)),
          msgpack::as_if<ImVec2>(msgpack::find(obj, "size"s), {0, 0})) {
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(2);

    const auto& data = mem_.data();

    pk.pack("size");
    pk.pack(std::make_pair(data.size.x, data.size.y));

    pk.pack("value"s);
    data.value.Serialize(pk);
  }
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    const auto& data = mem_.data();
    return std::make_unique<Imm>(env, Value(data.value), data.size);
  }

  void UpdateTree() noexcept override;
  void UpdateNode(const std::shared_ptr<Editor>&) noexcept override;
  void UpdateTypeChanger(bool mini = false) noexcept;
  void UpdateEditor() noexcept;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::DirItem, iface::Memento, iface::Node>(t).
        Select(this, &mem_);
  }

 private:
  class UniversalData final {
   public:
    UniversalData(Imm* o, Value&& v, ImVec2 sz) noexcept :
        value(std::move(v)), size(sz), owner_(o) {
    }

    bool operator==(const UniversalData& other) const noexcept {
      return
          value == other.value &&
          size.x == other.size.x && size.y == other.size.y;
    }
    void Restore(const UniversalData& src) noexcept {
      *this = src;
      owner_->Touch();
    }

    Value  value;
    ImVec2 size;

   private:
    Imm* owner_;
  };
  using Memento = SimpleMemento<UniversalData>;
  Memento mem_;

  OutSock          sock_out_;
  NodeLambdaInSock sock_clk_;
};
void Imm::UpdateTree() noexcept {
  UpdateTypeChanger();
  ImGui::SameLine();
  UpdateEditor();
}
void Imm::UpdateNode(const std::shared_ptr<Editor>& ctx) noexcept {
  ImGui::TextUnformatted("IMM:");
  ImGui::SameLine();
  UpdateTypeChanger(true);

  if (ImNodes::BeginInputSlot("CLK", 1)) {
    ImGui::AlignTextToFramePadding();
    gui::NodeSockPoint();
    ImGui::SameLine();
    if (ImGui::Button("CLK")) {
      Queue::main().Push([this, ctx]() { sock_clk_.Receive(ctx, {}); });
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
    gui::NodeSockPoint();
    ImNodes::EndSlot();
  }
}
void Imm::UpdateTypeChanger(bool mini) noexcept {
  auto& v = mem_.data().value;

  const char* type =
      v.isInteger()? "Int":
      v.isScalar()?  "Sca":
      v.isBoolean()? "Boo":
      v.isString()?  "Str": "XXX";
  mini? ImGui::SmallButton(type): ImGui::Button(type);

  gui::NodeCanvasResetZoom();
  if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
    if (ImGui::MenuItem("integer", nullptr, v.isInteger())) {
      v = Value::Integer {0};
      Touch();
    }
    if (ImGui::MenuItem("scalar", nullptr, v.isScalar())) {
      v = Value::Scalar {0};
      Touch();
    }
    if (ImGui::MenuItem("boolean", nullptr, v.isBoolean())) {
      v = Value::Boolean {false};
      Touch();
    }
    if (ImGui::MenuItem("string", nullptr, v.isString())) {
      v = ""s;
      Touch();
    }
    ImGui::EndPopup();
  }
  gui::NodeCanvasSetZoom();
}
void Imm::UpdateEditor() noexcept {
  const auto em = ImGui::GetFontSize();
  const auto fh = ImGui::GetFrameHeight();

  auto& data = mem_.data();
  auto& v    = data.value;

  ImGui::SameLine();
  if (v.isInteger()) {
    gui::ResizeGroup _("##resizer", &data.size, {4, fh/em}, {12, fh/em}, em);
    ImGui::SetNextItemWidth(data.size.x*em);
    if (ImGui::DragScalar("##editor", ImGuiDataType_S64, &v.integer())) {
      Touch();
    }

  } else if (v.isScalar()) {
    gui::ResizeGroup _("##resizer", &data.size, {4, fh/em}, {12, fh/em}, em);
    ImGui::SetNextItemWidth(data.size.x*em);
    if (ImGui::DragScalar("##editor", ImGuiDataType_Double, &v.scalar())) {
      Touch();
    }

  } else if (v.isBoolean()) {
    if (ImGui::Checkbox("##editor", &v.boolean())) {
      Touch();
    }

  } else if (v.isString()) {
    gui::ResizeGroup _("##resizer", &data.size, {4, fh/em}, {24, 24}, em);
    if (ImGui::InputTextMultiline("##editor", &v.stringUniq(), data.size*em)) {
      Touch();
    }

  } else {
    ImGui::TextUnformatted("UNKNOWN TYPE X(");
  }

  if (!ImGui::IsAnyItemActive() && data != mem_.commitData()) {
    mem_.Commit();
  }
}


// common part of Value/Name and Value/Pick
class NameOrPick : public File, public iface::Node {
 public:
  NameOrPick(TypeInfo* type, Env* env, std::vector<std::string>&& n = {}) noexcept :
      File(type, env), Node(Node::kMenu),
      memento_({this, std::move(n)}) {
  }

  void UpdateMenu(const std::shared_ptr<Editor>&) noexcept override;
  void UpdateNames(const std::shared_ptr<Editor>&) noexcept;
  virtual void UpdateSock(const std::string&) noexcept = 0;
  bool UpdateNamingMenu(const std::string&) noexcept;
  void UpdateAddMenu(size_t idx) noexcept;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Memento, iface::Node>(t).
        Select(this, &memento_);
  }

 protected:
  struct UniversalData final {
   public:
    UniversalData(NameOrPick* owner, std::vector<std::string>&& n) noexcept :
        names(std::move(n)), owner_(owner) {
    }
    void Restore(const UniversalData& other) {
      names = other.names;
      owner_->Rebuild();
    }

    // permanentized
    std::vector<std::string> names;

   private:
    NameOrPick* owner_;
  };
  UniversalData& udata() noexcept { return memento_.data(); }
  const UniversalData& udata() const noexcept { return memento_.data(); }


  virtual void Rebuild() noexcept = 0;
  virtual void Rename(Editor&, size_t idx, std::string&& name) noexcept {
    auto& udata = NameOrPick::udata();
    udata.names[idx] = std::move(name);
    Rebuild();
    memento_.Commit();
  }

 private:
  SimpleMemento<UniversalData> memento_;

  // volatile
  std::string new_name_;
};
void NameOrPick::UpdateMenu(const std::shared_ptr<Editor>&) noexcept {
  if (ImGui::BeginMenu("append")) {
    UpdateAddMenu(udata().names.size());
    ImGui::EndMenu();
  }
}
void NameOrPick::UpdateNames(const std::shared_ptr<Editor>& ctx) noexcept {
  auto& names = udata().names;

  const auto em = ImGui::GetFontSize();

  ImGui::BeginGroup();
  ImGui::PushItemWidth(6*em);
  {
    for (size_t idx = 0; idx < names.size();) {
      auto& name = names[idx++];

      ImGui::PushID(static_cast<int>(idx));
      ImGui::BeginGroup();
      UpdateSock(name);
      ImGui::EndGroup();
      if (ImGui::BeginPopupContextItem("##sock_menu")) {
        if (names.size() >= 2 && ImGui::MenuItem("remove")) {
          names.erase(names.begin() + static_cast<intmax_t>(--idx));
          Rebuild();
          memento_.Commit();
        }
        if (ImGui::BeginMenu("rename")) {
          if (UpdateNamingMenu(name)) {
            if (name != new_name_) {
              Rename(*ctx, idx-1, std::move(new_name_));
            }
          }
          ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("insert")) {
          UpdateAddMenu(idx-1);
          ImGui::EndMenu();
        }
        ImGui::EndPopup();
      }
      ImGui::PopID();
    }
  }
  ImGui::PopItemWidth();
  ImGui::EndGroup();
}
bool NameOrPick::UpdateNamingMenu(const std::string& before) noexcept {
  auto& names = udata().names;

  ImGui::SetKeyboardFocusHere();
  const bool submit = ImGui::InputTextWithHint(
      "##name_input", "new name...", &new_name_, ImGuiInputTextFlags_EnterReturnsTrue);

  const bool empty = new_name_.empty();
  const bool same  = new_name_ == before;
  const bool dup   = names.end() != std::find(names.begin(), names.end(), new_name_);
  if (empty) {
    ImGui::Bullet(); ImGui::TextUnformatted("empty name");
  }
  if (before.size() && same) {
    ImGui::Bullet(); ImGui::TextUnformatted("nothing changes");
  } else if (dup) {
    ImGui::Bullet(); ImGui::TextUnformatted("duplicated");
  }
  if (!empty && !dup && submit) {
    ImGui::CloseCurrentPopup();
    return !same;
  }
  return false;
}
void NameOrPick::UpdateAddMenu(size_t idx) noexcept {
  auto& names = udata().names;
  if (UpdateNamingMenu("")) {
    names.insert(names.begin()+static_cast<intmax_t>(idx), std::move(new_name_));
    Rebuild();
    memento_.Commit();
  }
}

class Name final : public NameOrPick {
 public:
  static inline TypeInfo kType = TypeInfo::New<Name>(
      "Value/Name", "name",
      {typeid(iface::Memento), typeid(iface::Node)});

  Name(Env* env, std::vector<std::string>&& n = {"praise_the_cat"}) noexcept :
      NameOrPick(&kType, env, std::move(n)), out_sock_(this, "out") {
    Rebuild();
  }

  Name(Env* env, const msgpack::object& obj)
  try : Name(env, obj.as<std::vector<std::string>>()) {
  } catch (msgpack::type_error&) {
    throw DeserializeException("broken Value/Name");
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack(udata().names);
  }
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    return std::make_unique<Name>(env, std::vector<std::string>(udata().names));
  }

  void UpdateNode(const std::shared_ptr<Editor>&) noexcept override;
  void UpdateSock(const std::string&) noexcept override;

 private:
  // volatile
  std::vector<std::unique_ptr<InSock>> in_socks_;
  OutSock out_sock_;


  void Rebuild() noexcept override {
    const auto& udata = NameOrPick::udata();

    in_socks_.resize(udata.names.size());
    in_.resize(udata.names.size());
    for (size_t i = 0; i < udata.names.size(); ++i) {
      in_socks_[i] = std::make_unique<CustomInSock>(this, &out_sock_, udata.names[i]);
      in_[i]       = in_socks_[i].get();
    }
    out_ = {&out_sock_};
    NotifySockChange();
  }
  void Rename(Editor& ctx, size_t idx, std::string&& name) noexcept override {
    auto socks = ctx.srcOf(in_[idx]);
    ctx.Unlink(*in_[idx]);
    NameOrPick::Rename(ctx, idx, std::move(name));
    for (auto sock : socks) ctx.Link(*in_[idx], *sock);
  }


  class CustomInSock final : public InSock {
   public:
    CustomInSock(Name* owner, OutSock* out, std::string_view name) noexcept :
        InSock(owner, name), out_(out) {
    }
    void Receive(const std::shared_ptr<Context>& ctx, Value&& v) noexcept {
      out_->Send(ctx, Value::Tuple { name(), std::move(v) });
    }
   private:
    OutSock* out_;
  };
};
void Name::UpdateNode(const std::shared_ptr<Editor>& ctx) noexcept {
  ImGui::TextUnformatted("NAME");

  UpdateNames(ctx);
  ImGui::SameLine();
  if (ImNodes::BeginOutputSlot("out", 1)) {
    gui::NodeSockPoint();
    ImNodes::EndSlot();
  }
}
void Name::UpdateSock(const std::string& name) noexcept {
  if (ImNodes::BeginInputSlot(name.c_str(), 1)) {
    gui::NodeSockPoint();
    ImGui::SameLine();
    ImGui::TextUnformatted(name.c_str());
    ImNodes::EndSlot();
  }
}

class Pick final : public NameOrPick {
 public:
  static inline TypeInfo kType = TypeInfo::New<Pick>(
      "Value/Pick", "name",
      {typeid(iface::Memento), typeid(iface::Node)});

  Pick(Env* env, std::vector<std::string>&& n = {"praise_the_cat"}) noexcept :
      NameOrPick(&kType, env, std::move(n)),
      in_sock_(this, "in", [this](auto& ctx, auto&& v) { Handle(ctx, std::move(v)); }) {
    Rebuild();
  }

  Pick(Env* env, const msgpack::object& obj)
  try : Pick(env, obj.as<std::vector<std::string>>()) {
  } catch (msgpack::type_error&) {
    throw DeserializeException("broken Value/Pick");
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack(udata().names);
  }
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    return std::make_unique<Pick>(env, std::vector<std::string>(udata().names));
  }

  void UpdateNode(const std::shared_ptr<Editor>&) noexcept override;
  void UpdateSock(const std::string&) noexcept override;

 private:
  // volatile
  std::vector<std::optional<OutSock>> out_socks_;
  NodeLambdaInSock in_sock_;

  float w_;


  void Rebuild() noexcept override {
    const auto& udata = NameOrPick::udata();

    out_socks_ = std::vector<std::optional<OutSock>>(udata.names.size());
    out_.resize(udata.names.size());

    for (size_t i = 0; i < udata.names.size(); ++i) {
      out_socks_[i].emplace(this, udata.names[i]);
      out_[i] = &*out_socks_[i];
    }
    in_ = {&in_sock_};
    NotifySockChange();
  }
  void Rename(Editor& ctx, size_t idx, std::string&& name) noexcept override {
    auto socks = ctx.dstOf(out_[idx]);
    ctx.Unlink(*out_[idx]);
    NameOrPick::Rename(ctx, idx, std::move(name));
    for (auto sock : socks) ctx.Link(*sock, *out_[idx]);
  }

  void Handle(const std::shared_ptr<Context>& ctx, Value&& v) noexcept
  try {
    const auto& tup = v.tuple();

    const auto& name  = tup[0].string();
    const auto& value = tup[1];

    if (auto sock = out(name)) {
      sock->Send(ctx, Value(value));
    }
  } catch (Exception& e) {
    ctx->Notify(this, e.msg());
  }
};
void Pick::UpdateNode(const std::shared_ptr<Editor>& ctx) noexcept {
  auto& names = udata().names;
  w_ = 0;
  for (const auto& name : names) {
    w_ = std::max(w_, ImGui::CalcTextSize(name.c_str()).x);
  }

  ImGui::TextUnformatted("PICK");

  if (ImNodes::BeginInputSlot("in", 1)) {
    gui::NodeSockPoint();
    ImNodes::EndSlot();
  }
  ImGui::SameLine();
  UpdateNames(ctx);
}
void Pick::UpdateSock(const std::string& name) noexcept {
  const auto tw = ImGui::CalcTextSize(name.c_str()).x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w_-tw);
  if (ImNodes::BeginOutputSlot(name.c_str(), 1)) {
    ImGui::TextUnformatted(name.c_str());
    ImGui::SameLine();
    gui::NodeSockPoint();
    ImNodes::EndSlot();
  }
}

} }  // namespace kingtaker
