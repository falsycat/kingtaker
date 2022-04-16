#include "kingtaker.hh"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <ImNodes.h>
#include <linalg.hh>

#include "iface/dir.hh"
#include "iface/memento.hh"
#include "iface/node.hh"

#include "util/gui.hh"
#include "util/history.hh"
#include "util/node.hh"
#include "util/notify.hh"
#include "util/ptr_selector.hh"
#include "util/value.hh"

namespace kingtaker {
namespace {

class Network : public File, public iface::DirItem, public iface::Node {
 public:
  static inline TypeInfo kType = TypeInfo::New<Network>(
      "Node/Network", "manages multiple Nodes and connections between them",
      {typeid(iface::DirItem)});

  Network(Env* env) noexcept :
      Network(env, Clock::now(), {}, {}, false, {0, 0}, 1.f) {
  }

  Network(Env* env, const msgpack::object& obj) :
      Network(env, obj, DeserializeNodes(env, msgpack::find(obj, "nodes"s))) {
  }
  void Serialize(Packer& pk) const noexcept override {
    std::unordered_map<Node*, size_t> idxmap;

    pk.pack_map(6);

    pk.pack("nodes"s);
    pk.pack_array(static_cast<uint32_t>(nodes_.size()));
    for (size_t i = 0; i < nodes_.size(); ++i) {
      auto& h = nodes_[i];
      h->Serialize(pk);
      idxmap[&h->node()] = i;
    }

    pk.pack("links"s);
    links_.Serialize(pk, idxmap);

    pk.pack("lastmod"s);
    pk.pack(lastmod_);

    pk.pack("shown"s);
    pk.pack(shown_);

    pk.pack("zoom"s);
    pk.pack(canvas_.Zoom);

    pk.pack("offset"s);
    pk.pack(std::make_tuple(canvas_.Offset.x, canvas_.Offset.y));
  }
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    std::unordered_map<Node*, Node*> nmap;

    NodeHolderList nodes;
    nodes.reserve(nodes_.size());

    size_t id = 0;
    for (auto& h : nodes_) {
      nodes.push_back(h->Clone(id++, env));
      nmap[&h->node()] = &nodes.back()->node();
    }

    return std::unique_ptr<File>(new Network(
        env, Clock::now(), std::move(nodes), links_.Clone(nmap),
        shown_, canvas_.Offset, canvas_.Zoom));
  }

  File* Find(std::string_view name) const noexcept override {
    try {
      size_t pos;
      const auto id = static_cast<size_t>(std::stoll(std::string(name), &pos));
      if (name.size() != pos) return nullptr;

      auto itr = std::find_if(nodes_.begin(), nodes_.end(),
                              [id](auto& e) { return e->id() == id; });
      return itr != nodes_.end()? &(*itr)->file(): nullptr;

    } catch (std::invalid_argument&) {
      return nullptr;
    } catch (std::out_of_range&) {
      return nullptr;
    }
  }

  void Update(RefStack& ref, Event& ev) noexcept override;
  void UpdateMenu(RefStack&) noexcept override;
  void UpdateCanvas(RefStack&) noexcept;
  void UpdateCanvasMenu(RefStack&, const ImVec2&) noexcept;
  template <typename T>
  void UpdateNewIO(const ImVec2& pos) noexcept;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::DirItem, iface::Node>(t).Select(this);
  }

 private:
  class NodeHolder;
  using NodeHolderList = std::vector<std::unique_ptr<NodeHolder>>;

  class InNode;
  class OutNode;

  // permanentized params
  NodeHolderList nodes_;
  NodeLinkStore  links_;
  size_t         next_id_ = 0;

  bool shown_;
  ImNodes::CanvasState canvas_;

  // volatile params
  std::shared_ptr<std::monostate> life_;

  std::unordered_set<InNode*>  in_nodes_;
  std::unordered_set<OutNode*> out_nodes_;

  std::unordered_map<Node*, NodeHolder*> hmap_;

  class EditorContext;
  std::shared_ptr<EditorContext> ctx_;

  ImVec2 canvas_size_;

  std::string io_new_name_;


  // private ctors
  Network(Env*             env,
          Time             lastmod,
          NodeHolderList&& nodes,
          NodeLinkStore&&  links,
          bool             shown,
          ImVec2           pos,
          float            zoom) noexcept :
      File(&kType, env, lastmod), DirItem(DirItem::kMenu), Node(Node::kNone),
      nodes_(std::move(nodes)), links_(std::move(links)), shown_(shown),
      life_(std::make_shared<std::monostate>()), history_(this) {
    canvas_.Zoom   = zoom;
    canvas_.Offset = pos;
    canvas_.Style.NodeRounding = 0.f;

    for (auto& node : nodes_) {
      node->SetUp(*this);
      next_id_ = std::max(next_id_, node->id()+1);
    }
  }
  Network(Env*                                            env,
          const msgpack::object&                          obj,
          std::pair<NodeHolderList, std::vector<Node*>>&& nodes)
  try : Network(env,
                msgpack::find(obj, "lastmod"s).as<Time>(),
                std::move(nodes.first),
                NodeLinkStore(msgpack::find(obj, "links"s), nodes.second),
                msgpack::find(obj, "shown"s).as<bool>(),
                msgpack::as_if<ImVec2>(msgpack::find(obj, "pos"s), {0, 0}),
                msgpack::find(obj, "zoom"s).as<float>()) {
  } catch (msgpack::type_error&) {
    throw DeserializeException("broken Node/Network");
  }
  static std::pair<NodeHolderList, std::vector<Node*>> DeserializeNodes(
      Env* env, const msgpack::object& obj) {
    if (obj.type != msgpack::type::ARRAY) throw msgpack::type_error();

    const auto n = obj.via.array.size;

    NodeHolderList nodes;
    nodes.reserve(n);

    std::vector<Node*> nmap;
    nmap.reserve(n);

    for (size_t i = 0; i < n; ++i) {
      nodes.push_back(std::make_unique<NodeHolder>(env, obj.via.array.ptr[i]));
      nmap.push_back(&nodes.back()->node());
    }
    return {std::move(nodes), std::move(nmap)};
  }


  NodeHolder* FindHolder(Node* n) const noexcept {
    auto itr = hmap_.find(n);
    return itr != hmap_.end()? itr->second: nullptr;
  }
  std::unique_ptr<NodeHolder> CreateNewHolderIf(
      std::unique_ptr<File>&& f, const ImVec2& pos) noexcept
  try {
    return std::make_unique<NodeHolder>(std::move(f), next_id_++, pos);
  } catch (Exception&) {
    return nullptr;
  }
  void Focus(RefStack& ref, NodeHolder* target) noexcept {
    for (auto& h : nodes_) h->select = false;
    target->select = true;

    // adjust offset to make the node displayed in center
    canvas_.Offset = (target->pos*canvas_.Zoom - canvas_size_/2.f)*-1.f;

    // focus the editor
    const auto id = ref.Stringify() + ": NetworkEditor";
    ImGui::SetWindowFocus(id.c_str());
    shown_ = true;
  }

  void RebuildSocks() noexcept {
    std::set<std::string> in_names;
    for (auto in : in_nodes_) in_names.insert(in->name());
    for (const auto& name : in_names) {
      if (in(name)) continue;
      in_.emplace_back(new CustomInSock(this, std::make_shared<SockMeta>(name)));
    }
    for (auto itr = in_.begin(); itr < in_.end();) {
      if (in_names.contains((*itr)->name())) {
        ++itr;
      } else {
        itr = in_.erase(itr);
      }
    }

    std::set<std::string> out_names;
    for (auto out : out_nodes_) out_names.insert(out->name());
    for (const auto& name : out_names) {
      if (out(name)) continue;
      out_.emplace_back(new OutSock(this, std::make_shared<SockMeta>(name)));
    }
    for (auto itr = out_.begin(); itr < out_.end();) {
      if (out_names.contains((*itr)->name())) {
        ++itr;
      } else {
        itr = out_.erase(itr);
      }
    }
  }


  // a wrapper for node files
  class NodeHolder final {
   public:
    NodeHolder(std::unique_ptr<File>&& f,
               size_t                  id,
               ImVec2                  p   = {0, 0},
               bool                    sel = false) :
        pos(p), select(sel),
        file_(std::move(f)),
        node_(File::iface<iface::Node>(file_.get())),
        memento_(File::iface<iface::Memento>(file_.get())),
        id_(id) {
      if (!node_) throw Exception("File doesn't have Node interface");
    }

    NodeHolder(Env* env, const msgpack::object& obj)
    try : NodeHolder(File::Deserialize(env, msgpack::find(obj, "file"s)),
                     msgpack::find(obj, "id"s).as<size_t>(),
                     msgpack::as_if<ImVec2>(msgpack::find(obj, "pos"s), {0, 0}),
                     msgpack::find(obj, "select"s).as<bool>()) {
    } catch (Exception& e) {
      throw DeserializeException("broken Node/Network NodeHolder: "+e.msg());
    }
    void Serialize(Packer& pk) const noexcept {
      pk.pack_map(4);

      pk.pack("id"s);
      pk.pack(id_);

      pk.pack("file"s);
      file_->SerializeWithTypeInfo(pk);

      pk.pack("pos"s);
      pk.pack(std::make_pair(pos.x, pos.y));

      pk.pack("select"s);
      pk.pack(select);
    }
    std::unique_ptr<NodeHolder> Clone(size_t id, Env* env) const noexcept {
      return std::make_unique<NodeHolder>(file_->Clone(env), id, pos, select);
    }

    void SetUp(Network& owner) noexcept {
      if (auto in = dynamic_cast<InNode*>(node_)) {
        owner.in_nodes_.insert(in);
      } else if (auto out = dynamic_cast<OutNode*>(node_)) {
        owner.out_nodes_.insert(out);
      }
      owner.hmap_[node_] = this;
      owner.RebuildSocks();
    }
    void TearDown(Network& owner) noexcept {
      if (auto in = dynamic_cast<InNode*>(node_)) {
        owner.in_nodes_.erase(in);
      } else if (auto out = dynamic_cast<OutNode*>(node_)) {
        owner.out_nodes_.erase(out);
      }
      owner.hmap_.erase(node_);
      owner.RebuildSocks();
    }

    std::unique_ptr<HistoryCommand> WatchMemento() noexcept;

    void Update(RefStack&, Event&) noexcept;
    void UpdateNode(Network&, RefStack&) noexcept;

    File& file() const noexcept { return *file_; }
    Node& node() const noexcept { return *node_; }

    size_t id() const noexcept { return id_; }

    // permanentized
    ImVec2 pos;
    bool select;

   private:
    std::unique_ptr<File> file_;
    Node*                 node_;
    iface::Memento*       memento_;

    // permanentized
    size_t id_;

    // volatile
    std::shared_ptr<iface::Memento::Tag> last_tag_;
  };


  // History interface implementation
  class History : public kingtaker::History<> {
   public:
    History(Network* o) : owner_(o) { }

    void AddNodeIf(std::unique_ptr<NodeHolder>&& h) noexcept {
      if (!h) return;
      AddNode(std::move(h));
    }
    void AddNode(std::unique_ptr<NodeHolder>&& h) noexcept;
    void RemoveNode(NodeHolder* h) noexcept;

    void AddSilently(std::unique_ptr<HistoryCommand>&& cmd) noexcept {
      tempq_.push_back(std::move(cmd));
    }
    void Queue(std::unique_ptr<HistoryCommand>&& cmd) noexcept {
      auto ptr = cmd.get();
      AddSilently(std::move(cmd));
      Queue::main().Push([ptr]() { ptr->Apply(); });
    }
    void EndFrame() noexcept {
      if (tempq_.empty()) return;
      kingtaker::History<>::AddSilently(
          std::make_unique<HistoryAggregateCommand>(std::move(tempq_)));
    }

   private:
    Network* owner_;

    std::vector<std::unique_ptr<HistoryCommand>> tempq_;

    class NodeSwapCommand;
  } history_;


  class InNode : public File, public iface::Node {
   public:
    static inline TypeInfo kType = TypeInfo::New<InNode>(
        "Node/Network/In", "input emitter in Node/Network", {});

    static inline const SockMeta kOut = {
      .name = "out", .type = SockMeta::kAny,
    };

    InNode(Env* env, std::string_view name) noexcept :
        File(&kType, env), Node(Node::kNone), name_(name) {
      out_.emplace_back(new OutSock(this, {&kOut, [](auto){}}));
    }

    InNode(Env* env, const msgpack::object& obj) : InNode(env, obj.as<std::string>()) {
    }
    void Serialize(Packer& pk) const noexcept override {
      pk.pack(name_);
    }
    std::unique_ptr<File> Clone(Env* env) const noexcept override {
      return std::make_unique<InNode>(env, name_);
    }

    void UpdateNode(RefStack&, const std::shared_ptr<Editor>&) noexcept override;

    void* iface(const std::type_index& t) noexcept override {
      return PtrSelector<iface::Node>(t).Select(this);
    }
    const std::string& name() const noexcept { return name_; }

   private:
    std::string name_;
  };
  class OutNode : public File, public iface::Node {
   public:
    static inline TypeInfo kType = TypeInfo::New<OutNode>(
        "Node/Network/Out", "output receiver in Node/Network", {});

    static inline const SockMeta kOut = {
      .name = "in", .type = SockMeta::kAny, .trigger = true,
    };

    OutNode(Env* env, std::string_view name) noexcept :
        File(&kType, env), Node(Node::kNone), name_(name) {
      in_.emplace_back(new InSock(this, {&kOut, [](auto){}}));
    }

    OutNode(Env* env, const msgpack::object& obj) : OutNode(env, obj.as<std::string>()) {
    }
    void Serialize(Packer& pk) const noexcept override {
      pk.pack(name_);
    }
    std::unique_ptr<File> Clone(Env* env) const noexcept override {
      return std::make_unique<OutNode>(env, name_);
    }

    void UpdateNode(RefStack&, const std::shared_ptr<Editor>&) noexcept override;

    void* iface(const std::type_index& t) noexcept override {
      return PtrSelector<iface::Node>(t).Select(this);
    }
    const std::string& name() const noexcept { return name_; }

   private:
    std::string name_;
  };


  // An impl of InSock that executes Network as lambda.
  class CustomInSock final : public InSock {
   public:
    CustomInSock(Network* owner, const std::shared_ptr<const SockMeta>& meta) noexcept :
        InSock(owner, meta), owner_(owner), life_(owner_->life_) {
    }

    void Receive(const std::shared_ptr<Context>& octx, Value&& v) noexcept override {
      if (life_.expired()) return;

      auto ictx = octx->data<LambdaContext>(owner_, octx, owner_);
      for (auto in : owner_->in_nodes_) {
        if (in->name() == name()) in->out(0)->Send(ictx, Value(v));
      }
    }

   private:
    Network* owner_;

    std::weak_ptr<std::monostate> life_;
  };


  // An impl of Context to execute Network as lambda.
  class LambdaContext : public Context, public Context::Data {
   public:
    LambdaContext(const std::shared_ptr<Context>& octx,
                  Network*                        owner) noexcept :
        Context(Path(owner->ctx_->basepath())),  // FIXME: ctx_ may not be created yet
        owner_(owner), life_(owner->life_), octx_(octx) {
    }

    void ObserveReceive(const InSock& in, const Value& v) noexcept override {
      if (life_.expired()) return;

      auto itr = owner_->out_nodes_.find(dynamic_cast<OutNode*>(in.owner()));
      if (itr == owner_->out_nodes_.end()) return;

      const auto& out_sock = owner_->out((*itr)->name());
      if (!out_sock) return;

      out_sock->Send(octx_, Value(v));
    }

    std::span<const std::shared_ptr<InSock>> dstOf(const OutSock* out) const noexcept override {
      if (life_.expired()) return {};
      return owner_->links_.dstOf(out);
    }
    std::span<const std::shared_ptr<OutSock>> srcOf(const InSock* in) const noexcept override {
      if (life_.expired()) return {};
      return owner_->links_.srcOf(in);
    }

   private:
    Network* owner_;

    std::weak_ptr<std::monostate> life_;

    std::shared_ptr<Context> octx_;
  };

  // An impl of Node::Editor for Network
  class EditorContext final : public Editor {
   public:
    EditorContext(File::Path&& basepath, Network* o) noexcept :
        Editor(std::move(basepath)), owner_(o), life_(o->life_) {
    }

    void Link(const std::shared_ptr<InSock>& in, const std::shared_ptr<OutSock>& out) noexcept override {
      if (life_.expired()) return;
      owner_->history_.Queue(std::make_unique<NodeLinkStore::SwapCommand>(
          &owner_->links_, NodeLinkStore::SwapCommand::kLink, *in, *out));
    }
    void Unlink(const InSock& in, const OutSock& out) noexcept override {
      if (life_.expired()) return;
      owner_->history_.Queue(std::make_unique<NodeLinkStore::SwapCommand>(
          &owner_->links_, NodeLinkStore::SwapCommand::kUnlink, in, out));
    }

    std::span<const std::shared_ptr<InSock>> dstOf(const OutSock* out) const noexcept override {
      if (life_.expired()) return {};
      return owner_->links_.dstOf(out);
    }
    std::span<const std::shared_ptr<OutSock>> srcOf(const InSock* in) const noexcept override {
      if (life_.expired()) return {};
      return owner_->links_.srcOf(in);
    }

   private:
    Network* owner_;

    std::weak_ptr<std::monostate> life_;
  };
};

void Network::Update(RefStack& ref, Event& ev) noexcept {
  auto path = ref.GetFullPath();

  // update editor context
  if (!ctx_ || ctx_->basepath() != path) {
    if (ctx_) {
      notify::Info(path, this, "path change detected, editor context is cleared");
    }
    ctx_ = std::make_shared<EditorContext>(std::move(path), this);
  }

  // update children
  for (auto& h : nodes_) {
    if (ev.IsFocused(&h->file())) Focus(ref, h.get());
    h->Update(ref, ev);
  }

  // display window
  const auto size = ImVec2(24.f, 24.f)*ImGui::GetFontSize();
  ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);

  constexpr auto kFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
  if (gui::BeginWindow(this, "NetworkEditor", ref, ev, &shown_, kFlags)) {
    canvas_size_ = ImGui::GetWindowSize();
    UpdateCanvas(ref);
  }
  gui::EndWindow();
}
void Network::UpdateMenu(RefStack&) noexcept {
  ImGui::MenuItem("NetworkEditor", nullptr, &shown_);
}
void Network::UpdateCanvas(RefStack& ref) noexcept {
  const auto pos = ImGui::GetCursorScreenPos();

  ImNodes::BeginCanvas(&canvas_);
  gui::NodeCanvasSetZoom();

  gui::NodeCanvasResetZoom();
  if (ImGui::BeginPopupContextItem()) {
    UpdateCanvasMenu(ref, pos);
    ImGui::EndPopup();
  }
  gui::NodeCanvasSetZoom();

  // update children
  for (auto& h : nodes_) {
    h->UpdateNode(*this, ref);
  }

  // handle existing connections
  for (auto& src : links_.out()) {
    for (const auto& dst : src.second.others()) {
      auto srch = FindHolder(src.second.self()->owner());
      auto srcs = src.second.self()->name().c_str();
      auto dsth = FindHolder(dst->owner());
      auto dsts = dst->name().c_str();
      if (!srch || !dsth) continue;

      if (!ImNodes::Connection(dsth, dsts, srch, srcs)) {
        ctx_->Unlink(*dst, *src.second.self());
      }
    }
  }

  // handle new connection
  void* inptr;
  void* outptr;
  const char* srcs;
  const char* dsts;
  if (ImNodes::GetNewConnection(&inptr, &dsts, &outptr, &srcs)) {
    auto dstn = reinterpret_cast<NodeHolder*>(inptr);
    auto srcn = reinterpret_cast<NodeHolder*>(outptr);

    auto src = srcn->node().out(srcs);
    auto dst = dstn->node().in(dsts);
    if (src && dst) ctx_->Link(dst, src);
  }

  // detect memento changes
  bool update = false;
  for (auto& h : nodes_) {
    if (auto cmd = h->WatchMemento()) {
      history_.AddSilently(std::move(cmd));
      update = true;
    }
  }
  if (update) links_.CleanUp();
  history_.EndFrame();

  gui::NodeCanvasResetZoom();
  ImNodes::EndCanvas();
}
void Network::UpdateCanvasMenu(RefStack&, const ImVec2& winpos) noexcept {
  const auto pos =
      (ImGui::GetWindowPos()-winpos) / canvas_.Zoom - canvas_.Offset;

  if (ImGui::BeginMenu("New")) {
    for (auto& p : File::registry()) {
      auto& t = *p.second;
      if (!t.factory() || !t.IsImplemented<Node>()) continue;
      if (ImGui::MenuItem(t.name().c_str())) {
        history_.AddNodeIf(CreateNewHolderIf(t.Create(&env()), pos));
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", t.desc().c_str());
      }
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Input")) {
      UpdateNewIO<InNode>(pos);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Output")) {
      UpdateNewIO<OutNode>(pos);
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }

  ImGui::Separator();
  if (ImGui::MenuItem("Undo")) {
    history_.UnDo();
  }
  if (ImGui::MenuItem("Redo")) {
    history_.ReDo();
  }

  ImGui::Separator();
  if (ImGui::MenuItem("Clear history")) {
    history_.Clear();
  }
  if (ImGui::MenuItem("Clear entire context")) {
    Queue::main().Push([this]() { ctx_ = nullptr; });
  }
}
template <typename T>
void Network::UpdateNewIO(const ImVec2& pos) noexcept {
  constexpr auto kFlags =
      ImGuiInputTextFlags_EnterReturnsTrue |
      ImGuiInputTextFlags_AutoSelectAll;

  static const char* kHint = "enter to add...";

  ImGui::SetKeyboardFocusHere();
  const bool submit =
      ImGui::InputTextWithHint("##newIO", kHint, &io_new_name_, kFlags);

  const bool empty = io_new_name_.empty();
  if (empty) {
    ImGui::Bullet();
    ImGui::TextUnformatted("empty name");
  }
  if (submit && !empty) {
    history_.AddNodeIf(CreateNewHolderIf(std::make_unique<T>(&env(), io_new_name_), pos));
    io_new_name_ = "";
    ImGui::CloseCurrentPopup();
  }
}
void Network::NodeHolder::Update(RefStack& ref, Event& ev) noexcept {
  ref.Push({std::to_string(id_), file_.get()});
  ImGui::PushID(file_.get());

  file_->Update(ref, ev);

  ImGui::PopID();
  ref.Pop();
}
void Network::NodeHolder::UpdateNode(Network& owner, RefStack& ref) noexcept {
  ref.Push({std::to_string(id_), file_.get()});
  ImGui::PushID(file_.get());

  if (ImNodes::BeginNode(this, &pos, &select)) {
    node_->UpdateNode(ref, owner.ctx_);
  }
  ImNodes::EndNode();

  gui::NodeCanvasResetZoom();
  if (ImGui::BeginPopupContextItem()) {
    if (ImGui::MenuItem("Clone")) {
      owner.history_.AddNodeIf(Clone(owner.next_id_++, &owner.env()));
    }
    if (ImGui::MenuItem("Remove")) {
      owner.history_.RemoveNode(this);
    }
    if (node_->flags() & Node::kMenu) {
      ImGui::Separator();
      node_->UpdateMenu(ref, owner.ctx_);
    }
    ImGui::EndPopup();
  }
  gui::NodeCanvasSetZoom();

  ImGui::PopID();
  ref.Pop();
}

void Network::InNode::UpdateNode(RefStack&, const std::shared_ptr<Editor>&) noexcept {
  ImGui::Text("IN> %s", name_.c_str());

  ImGui::SameLine();
  if (ImNodes::BeginOutputSlot("out", 1)) {
    gui::NodeSocket();
    ImNodes::EndSlot();
  }
}
void Network::OutNode::UpdateNode(RefStack&, const std::shared_ptr<Editor>&) noexcept {
  if (ImNodes::BeginInputSlot("in", 1)) {
    gui::NodeSocket();
    ImNodes::EndSlot();
  }

  ImGui::SameLine();
  ImGui::Text("%s >OUT", name_.c_str());
}

std::unique_ptr<HistoryCommand> Network::NodeHolder::WatchMemento() noexcept {
  if (!memento_) return nullptr;

  class MementoCommand final : public HistoryCommand {
   public:
    MementoCommand(NodeHolder* h, const std::shared_ptr<iface::Memento::Tag>& tag) noexcept :
        holder_(h), tag_(tag) {
    }
    void Apply() override { Exec(); }
    void Revert() override { Exec(); }
   private:
    NodeHolder* holder_;
    std::shared_ptr<iface::Memento::Tag> tag_;

    void Exec() noexcept {
      if (holder_->last_tag_.get() == tag_.get()) return;
      holder_->last_tag_ = tag_;
      tag_ = tag_->Restore();
    }
  };

  const auto ptag = std::move(last_tag_);
  last_tag_ = memento_->Save();

  if (ptag && ptag.get() != last_tag_.get()) {
    return std::make_unique<MementoCommand>(this, ptag);
  }
  return nullptr;
}

// a command for node creation or removal
class Network::History::NodeSwapCommand : public HistoryCommand {
 public:
  NodeSwapCommand(Network* o, std::unique_ptr<NodeHolder>&& h) noexcept :
      owner_(o), holder_(std::move(h)), ref_(holder_.get()) {
  }
  NodeSwapCommand(Network* o, NodeHolder* h) noexcept : owner_(o), ref_(h) {
  }

  void Exec() {
    auto& nodes = owner_->nodes_;
    if (holder_) {
      holder_->SetUp(*owner_);
      nodes.push_back(std::move(holder_));
    } else {
      auto itr = std::find_if(nodes.begin(), nodes.end(),
                              [ref = ref_](auto& x) { return x.get() == ref; });
      if (itr == nodes.end()) {
        throw Exception("NodeSwapCommand history collapsed");
      }
      holder_ = std::move(*itr);
      holder_->TearDown(*owner_);
      nodes.erase(itr);
    }
  }
  void Apply() override { Exec(); }
  void Revert() override { Exec(); }

 private:
  Network*                    owner_;
  std::unique_ptr<NodeHolder> holder_;
  NodeHolder*                 ref_;
};
void Network::History::AddNode(std::unique_ptr<NodeHolder>&& h) noexcept {
  Queue(std::make_unique<NodeSwapCommand>(owner_, std::move(h)));
}
void Network::History::RemoveNode(NodeHolder* h) noexcept {
  for (const auto& in : h->node().in()) {
    for (const auto& out : owner_->links_.srcOf(in.get())) {
      Queue(std::make_unique<NodeLinkStore::SwapCommand>(
              &owner_->links_, NodeLinkStore::SwapCommand::kUnlink, *in, *out));
    }
  }
  for (const auto& out : h->node().out()) {
    for (const auto& in : owner_->links_.dstOf(out.get())) {
      Queue(std::make_unique<NodeLinkStore::SwapCommand>(
              &owner_->links_, NodeLinkStore::SwapCommand::kUnlink, *in, *out));
    }
  }
  Queue(std::make_unique<NodeSwapCommand>(owner_, std::move(h)));
}


class Call final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<Call>;

  static inline TypeInfo kType = TypeInfo::New<Owner>(
      "Node/Call", "redirect input to a specific Node on filesystem with sub context",
      {typeid(iface::Node)});

  static inline const std::vector<SockMeta> kInSocks = {
    { .name = "clear", .type = SockMeta::kPulse, .trigger = true, },
    { .name = "path",  .type = SockMeta::kStringPath, },
    { .name = "send",  .type = SockMeta::kTuple, .trigger = true, },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { .name = "recv", .type = SockMeta::kTuple, },
  };

  Call() = delete;
  Call(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), octx_(ctx) {
  }

  std::string title() const noexcept {
    return ictx_.expired()? "CALL": "CALL*";
  }

  void Handle(size_t idx, Value&& v) {
    switch (idx) {
    case 0:
      Clear();
      return;
    case 1:
      path_ = File::ParsePath(v.string());
      return;
    case 2:
      Send(std::move(v));
      return;
    }
    assert(false);
  }
  void Clear() noexcept {
    auto ictx = ictx_.lock();
    if (ictx) ictx->Attach(nullptr);
    ictx_.reset();
    path_ = {};
  }
  void Send(Value&& v) {
    auto f = &*RefStack().Resolve(owner_->path()).Resolve(path_);
    if (f == owner_) throw Exception("self reference");

    auto n = File::iface<iface::Node>(f);
    if (!n) throw Exception("target doesn't have Node interface");

    const auto& tup   = v.tuple(2);
    const auto& name  = tup[0].string();
    const auto& value = tup[1];

    auto sock = n->in(name);
    if (!sock) throw Exception("unknown input: "+name);

    auto ictx = ictx_.lock();
    if (ictx && ictx->target() != n) {
      ictx->Attach(nullptr);
      ictx = nullptr;
    }
    if (!ictx) {
      ictx = std::make_shared<NodeRedirectContext>(octx_.lock(), owner_->out(0), n);
    }
    ictx_ = ictx;

    auto task = [sock, ictx, v = value]() mutable {
      sock->Receive(ictx, std::move(v));
    };
    Queue::sub().Push(std::move(task));
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> octx_;

  File::Path path_;

  std::weak_ptr<NodeRedirectContext> ictx_;
};


class Cache final : public File, public iface::DirItem {
 public:
  static inline TypeInfo kType = TypeInfo::New<Cache>(
      "Node/Cache", "stores execution result of Node",
      {typeid(iface::DirItem)});

  Cache(Env* env) noexcept :
      File(&kType, env), DirItem(DirItem::kMenu | DirItem::kTooltip),
      store_(std::make_shared<Store>()) {
  }

  Cache(Env* env, const msgpack::object&) noexcept :
      Cache(env) {
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_nil();
  }
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    return std::make_unique<Cache>(env);
  }

  void Update(RefStack& ref, Event&) noexcept override {
    basepath_ = ref.GetFullPath();
  }
  void UpdateMenu(RefStack&) noexcept override {
    if (ImGui::MenuItem("drop all cache")) store_->DropAll();
  }
  void UpdateTooltip(RefStack&) noexcept override {
    ImGui::Indent();
    ImGui::Text("store size  : %zu", store_->size());
    ImGui::Text("try/hit/miss: %zu/%zu/%zu", try_cnt_, hit_cnt_, try_cnt_-hit_cnt_);
    if (try_cnt_ > 0) {
      ImGui::Text("hit rate    : %f%%",
                  static_cast<float>(hit_cnt_)/static_cast<float>(try_cnt_)*100.f);
    }
    ImGui::Unindent();
  }

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::DirItem>(t).Select(this);
  }

 private:
  Path basepath_;

  class Store;
  std::shared_ptr<Store> store_;

  size_t try_cnt_ = 0;
  size_t hit_cnt_ = 0;


  using Param = std::pair<std::string, Value>;

  class StoreItem final {
   public:
    friend class Store;

    using Observer = std::function<void(std::string_view, const Value&)>;

    void Observe(Observer&& obs) noexcept {
      for (const auto& out : out_) {
        obs(out.first, out.second);
      }
      obs_.push_back(obs);
    }
    void Set(const std::string& name, Value&& v) noexcept {
      out_.emplace_back(name, v);
      for (const auto& obs : obs_) {
        obs(name, v);
      }
    }

    // after calling this, Set() cannot be called
    void Finish() noexcept {
      obs_.clear();
    }

    const std::vector<Param>& in() const noexcept { return in_; }
    const std::vector<Param>& out() const noexcept { return out_; }

   private:
    StoreItem() = delete;
    StoreItem(std::vector<Param>&& in) noexcept : in_(std::move(in)) {
    }

    std::vector<Param> in_, out_;

    std::vector<Observer> obs_;
  };
  class Store final {
   public:
    Store() noexcept { }

    std::shared_ptr<StoreItem> Find(const std::vector<Param>& in) const noexcept {
      for (const auto& item : items_) {
        if (in == item->in()) return item;
      }
      return nullptr;
    }
    const std::shared_ptr<StoreItem>& Allocate(std::vector<Param>&& in) noexcept {
      items_.emplace_back(new StoreItem(std::move(in)));
      return items_.back();
    }
    void DropAll() noexcept {
      items_.clear();
    }

    size_t size() const noexcept { return items_.size(); }

   private:
    std::deque<std::shared_ptr<StoreItem>> items_;
  };

  // Node::Context that observes output of cache target.
  class InnerContext final : public iface::Node::Context {
   public:
    using Node = iface::Node;

    InnerContext(const std::shared_ptr<Context>& octx,
                 Node*                           target,
                 const std::weak_ptr<StoreItem>& item) noexcept :
        Context(Path(octx->basepath())),
        octx_(octx), target_(target), item_(item) {
    }
    ~InnerContext() {
      auto item = item_.lock();
      if (item) item->Finish();
    }

    void ObserveSend(const Node::OutSock& sock, const Value& v) noexcept override {
      if (sock.owner() != target_) return;

      auto item = item_.lock();
      if (!item) return;

      auto task = [item, name = sock.name(), v = v]() mutable {
        item->Set(name, std::move(v));
      };
      Queue::sub().Push(std::move(task));
    }

    std::span<const std::shared_ptr<Node::InSock>>
        dstOf(const Node::OutSock* out) const noexcept override {
      return octx_->dstOf(out);
    }
    std::span<const std::shared_ptr<Node::OutSock>>
        srcOf(const Node::InSock* in) const noexcept override {
      return octx_->srcOf(in);
    }

   private:
    std::shared_ptr<Node::Context> octx_;

    Node* target_;

    std::weak_ptr<StoreItem> item_;
  };


  class Call final : public LambdaNodeDriver {
   public:
    using Owner = LambdaNode<Call>;

    static inline TypeInfo kType = TypeInfo::New<Owner>(
        "Node/Cache/Call", "redirect input or emit cached value",
        {typeid(iface::Node)});

    static inline const std::vector<SockMeta> kInSocks = {
      { .name = "clear",      .type = SockMeta::kPulse, .trigger = true, },
      { .name = "node_path",  .type = SockMeta::kStringPath, },
      { .name = "cache_path", .type = SockMeta::kStringPath, },
      { .name = "params",     .type = SockMeta::kTuple, .multi = true, },
      { .name = "exec",       .type = SockMeta::kPulse, .trigger = true, },
    };
    static inline const std::vector<SockMeta> kOutSocks = {
      { .name = "out", .type = SockMeta::kTuple, },
    };

    Call(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
        owner_(o), ctx_(ctx) {
    }

    std::string title() const noexcept {
      return "CACHE CALL";
    }

    void Handle(size_t idx, Value&& v) {
      switch (idx) {
      case 0:
        Clear();
        return;
      case 1:
        node_path_ = File::ParsePath(v.string());
        return;
      case 2:
        cache_path_ = File::ParsePath(v.string());
        return;
      case 3:
        SetParam(std::move(v));
        return;
      case 4:
        Exec();
        return;
      }
      assert(false);
    }
    void Clear() noexcept {
      node_path_  = {};
      cache_path_ = {};
      params_     = {};
    }
    void SetParam(Value&& v) {
      const auto& tup = v.tuple(2);
      params_.emplace_back(tup[0].string(), tup[1]);
    }
    void Exec() {
      auto ctx = ctx_.lock();
      auto c   = GetCacheFile();
      auto n   = GetNode();
      ++c->try_cnt_;

      std::weak_ptr<OutSock> wout = owner_->out(0);
      auto obs = [ctx, wout](auto name, auto& value) {
        auto out = wout.lock();
        if (out) out->Send(ctx, Value::Tuple { std::string(name), value });
      };

      // use cache if found
      auto cache = c->store_->Find(params_);
      if (cache) {
        ++c->hit_cnt_;
        cache->Observe(std::move(obs));
        return;
      }

      // allocate new cache
      cache = c->store_->Allocate(std::vector<Param>(params_));
      cache->Observe(std::move(obs));

      // redirect input
      auto ictx = std::make_shared<InnerContext>(ctx, n, cache);
      for (const auto& param : params_) {
        try {
          const auto in = n->in(param.first);
          if (!in) throw Exception("no such socket found: "+param.first);
          in->Receive(ictx, Value(param.second));
        } catch (Exception& e) {
          notify::Warn(owner_->path(), owner_, e.msg());
        }
      }
    }

   private:
    Owner* owner_;

    std::weak_ptr<Context> ctx_;

    Path node_path_;
    Path cache_path_;

    std::vector<Param> params_;


    Node* GetNode() {
      auto f = &*RefStack().Resolve(ctx_.lock()->basepath()).Resolve(node_path_);
      auto n = File::iface<Node>(f);
      if (!n) throw Exception("target file doesn't have Node interface");
      return n;
    }
    Cache* GetCacheFile() {
      auto c = dynamic_cast<Cache*>(
          &*RefStack().Resolve(ctx_.lock()->basepath()).Resolve(cache_path_));
      if (!c) throw Exception("file is not Node/Cache");
      return c;
    }
  };
};

} }  // namespace kingtaker
