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
#include "util/life.hh"
#include "util/memento.hh"
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
      Network(env, Clock::now(), {}, std::make_unique<NodeLinkStore>(), false, {0, 0}, 1.f) {
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
    links_->Serialize(pk, idxmap);

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
        env, Clock::now(), std::move(nodes), links_->Clone(nmap),
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

  // permanentized params
  NodeHolderList                 nodes_;
  std::unique_ptr<NodeLinkStore> links_;
  size_t                         next_id_ = 0;

  bool shown_;
  ImNodes::CanvasState canvas_;

  // volatile params
  Life life_;

  class InNode;
  class OutNode;
  std::unordered_set<InNode*>  in_nodes_;
  std::unordered_set<OutNode*> out_nodes_;

  class CustomInSock;
  std::vector<std::unique_ptr<OutSock>>      out_socks_;
  std::vector<std::unique_ptr<CustomInSock>> in_socks_;

  std::unordered_map<Node*, NodeHolder*> hmap_;

  class EditorContext;
  std::shared_ptr<EditorContext> ctx_;

  ImVec2 canvas_size_;

  std::string io_new_name_;


  // private ctors
  Network(Env*                             env,
          Time                             lastmod,
          NodeHolderList&&                 nodes,
          std::unique_ptr<NodeLinkStore>&& links,
          bool                             shown,
          ImVec2                           pos,
          float                            zoom) noexcept :
      File(&kType, env, lastmod), DirItem(DirItem::kMenu), Node(Node::kNone),
      nodes_(std::move(nodes)), links_(std::move(links)), shown_(shown),
      history_(this) {
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
                std::make_unique<NodeLinkStore>(msgpack::find(obj, "links"s), nodes.second),
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

  template <typename T>
  void SyncSocks(const auto& nodes, auto& socks, auto& sock_ptrs) noexcept {
    std::set<std::string> names;
    for (auto in : nodes) names.insert(in->name());

    auto term_idx = std::remove_if(
        socks.begin(), socks.end(),
        [&names](auto& x) { return !names.contains(x->name()); }) - socks.begin();

    sock_ptrs.clear();
    socks.resize(names.size());

    size_t idx = 0;
    for (const auto& name : names) {
      auto itr = std::find_if(socks.begin(), socks.end(),
                              [&name](auto& x) { return x && x->name() == name; });
      if (itr == socks.end()) {
        assert(static_cast<size_t>(term_idx) < socks.size());
        itr  = socks.begin() + (term_idx++);
        *itr = std::make_unique<T>(this, name);
      }
      std::swap(socks[idx], *itr);
      sock_ptrs.push_back(itr->get());
    }
  }
  void RebuildSocks() noexcept {
    SyncSocks<CustomInSock>(in_nodes_, in_socks_, in_);
    SyncSocks<CustomOutSock>(out_nodes_, out_socks_, out_);
    NotifySockChange();
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

    void Update(Network&, RefStack&, Event&) noexcept;
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


  // An impl of InSock that executes Network as lambda.
  class CustomInSock final : public InSock {
   public:
    CustomInSock(Network* owner, const std::string& name) noexcept :
        InSock(owner, &meta_),
        owner_(owner), life_(owner_->life_), meta_({.name = name}) {
    }

    void Receive(const std::shared_ptr<Context>& octx, Value&& v) noexcept override {
      if (!*life_) return;
      auto ictx = octx->data<LambdaContext>(owner_, owner_, octx);
      for (auto in : owner_->in_nodes_) {
        if (in->name() == name()) in->out(0).Send(ictx, Value(v));
      }
    }

   private:
    Network* owner_;

    Life::Ref life_;

    SockMeta meta_;
  };
  class CustomOutSock final : public OutSock {
   public:
    CustomOutSock(Network* owner, const std::string& name) noexcept :
        OutSock(owner, &meta_), meta_({.name = name}) {
    }

   private:
    SockMeta meta_;
  };


  // An impl of Context to execute Network as lambda.
  class LambdaContext : public Context, public Context::Data {
   public:
    LambdaContext(Network* owner, const std::shared_ptr<Context>& octx) noexcept :
        Context(Path(owner->ctx_->basepath())),  // FIXME: ctx_ may not be created yet
        owner_(owner), life_(owner_->life_), octx_(octx) {
    }

    void ObserveReceive(const InSock& in, const Value& v) noexcept override {
      if (!*life_) return;
      auto itr = owner_->out_nodes_.find(dynamic_cast<OutNode*>(in.owner()));
      if (itr == owner_->out_nodes_.end()) return;

      const auto& out_sock = owner_->out((*itr)->name());
      if (!out_sock) return;

      out_sock->Send(octx_, Value(v));
    }

    std::vector<InSock*> dstOf(const OutSock* out) const noexcept override {
      if (!*life_) return {};
      return owner_->links_->dstOf(out);
    }
    std::vector<OutSock*> srcOf(const InSock* in) const noexcept override {
      if (!*life_) return {};
      return owner_->links_->srcOf(in);
    }

   private:
    Network* owner_;

    Life::Ref life_;

    std::shared_ptr<Context> octx_;
  };

  // An impl of Node::Editor for Network
  class EditorContext final : public Editor {
   public:
    EditorContext(Path&& basepath, Network* o) noexcept :
        Editor(std::move(basepath)), owner_(o), life_(o->life_) {
    }

    void Link(const InSock& in, const OutSock& out) noexcept override {
      if (!*life_) return;
      owner_->history_.Queue(std::make_unique<NodeLinkStore::SwapCommand>(
          owner_->links_.get(), NodeLinkStore::SwapCommand::kLink, in, out));
    }
    void Unlink(const InSock& in, const OutSock& out) noexcept override {
      if (!*life_) return;
      owner_->history_.Queue(std::make_unique<NodeLinkStore::SwapCommand>(
          owner_->links_.get(), NodeLinkStore::SwapCommand::kUnlink, in, out));
    }

    std::vector<InSock*> dstOf(const OutSock* out) const noexcept override {
      if (!*life_) return {};
      return owner_->links_->dstOf(out);
    }
    std::vector<OutSock*> srcOf(const InSock* in) const noexcept override {
      if (!*life_) return {};
      return owner_->links_->srcOf(in);
    }

   private:
    Network* owner_;

    Life::Ref life_;
  };


  class InNode : public File, public iface::Node {
   public:
    static inline TypeInfo kType = TypeInfo::New<InNode>(
        "Node/Network/In", "input emitter in Node/Network", {});

    static inline const SockMeta kOut = {
      .name = "out", .type = SockMeta::kAny,
    };

    InNode(Env* env, std::string_view name) noexcept :
        File(&kType, env), Node(Node::kNone), sock_(this, &kOut), name_(name) {
      out_ = {&sock_};
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
    OutSock sock_;

    std::string name_;
  };


  class OutNode : public File, public iface::Node {
   public:
    static inline TypeInfo kType = TypeInfo::New<OutNode>(
        "Node/Network/Out", "output receiver in Node/Network", {});

    static inline const SockMeta kIn = {
      .name = "in", .type = SockMeta::kAny, .trigger = true,
    };

    OutNode(Env* env, std::string_view name) noexcept :
        File(&kType, env), Node(Node::kNone), sock_(this, &kIn), name_(name) {
      in_ = {&sock_};
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
    InSock sock_;

    std::string name_;
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
    h->Update(*this, ref, ev);
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
  for (auto& link : links_->items()) {
    auto srch = FindHolder(link.out.node);
    auto srcs = link.out.name.c_str();
    auto dsth = FindHolder(link.in.node);
    auto dsts = link.in.name.c_str();
    if (!srch || !dsth) continue;

    if (!ImNodes::Connection(dsth, dsts, srch, srcs)) {
      ctx_->Unlink(*link.in.sock, *link.out.sock);
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
    if (src && dst) ctx_->Link(*dst, *src);
  }

  // handle dead links
  const auto deads = links_->TakeDeadLinks();
  for (auto& link : deads) {
    history_.AddSilently(std::make_unique<NodeLinkStore::SwapCommand>(
            links_.get(), NodeLinkStore::SwapCommand::kUnlink,
            link.in.node,  link.in.name,
            link.out.node, link.out.name));
  }

  // detect memento changes
  for (auto& h : nodes_) {
    if (auto cmd = h->WatchMemento()) {
      history_.AddSilently(std::move(cmd));
    }
  }
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
void Network::NodeHolder::Update(Network& owner, RefStack& ref, Event& ev) noexcept {
  ref.Push({std::to_string(id_), file_.get()});
  ImGui::PushID(file_.get());

  file_->Update(ref, ev);
  node_->Update(ref, owner.ctx_);

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
    gui::NodeSockPoint();
    ImNodes::EndSlot();
  }
}
void Network::OutNode::UpdateNode(RefStack&, const std::shared_ptr<Editor>&) noexcept {
  if (ImNodes::BeginInputSlot("in", 1)) {
    gui::NodeSockPoint();
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
    for (const auto& out : owner_->links_->srcOf(in)) {
      Queue(std::make_unique<NodeLinkStore::SwapCommand>(
              owner_->links_.get(), NodeLinkStore::SwapCommand::kUnlink, *in, *out));
    }
  }
  for (const auto& out : h->node().out()) {
    for (const auto& in : owner_->links_->dstOf(out)) {
      Queue(std::make_unique<NodeLinkStore::SwapCommand>(
              owner_->links_.get(), NodeLinkStore::SwapCommand::kUnlink, *in, *out));
    }
  }
  Queue(std::make_unique<NodeSwapCommand>(owner_, std::move(h)));
}


class Call final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<Call>;

  static inline TypeInfo kType = TypeInfo::New<Owner>(
      "Node/Call", "redirects input to a specific Node on filesystem with sub context",
      {typeid(iface::Node)});

  static inline const std::vector<SockMeta> kInSocks = {
    { .name = "path",  .type = SockMeta::kStringPath, },
    { .name = "send",  .type = SockMeta::kNamedValue, .trigger = true, },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { .name = "recv", .type = SockMeta::kNamedValue, },
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
      path_ = File::ParsePath(v.string());
      return;
    case 1:
      Send(std::move(v));
      return;
    }
    assert(false);
  }
  void Send(Value&& v) {
    auto octx = octx_.lock();

    auto f = &*RefStack().Resolve(octx->basepath()).Resolve(path_);
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
      ictx = std::make_shared<NodeRedirectContext>(octx, owner_->sharedOut(0), n);
    }
    ictx_ = ictx;

    auto task = [sock, ictx, v = value]() mutable {
      sock->Receive(ictx, std::move(v));
    };
    Queue::sub().Push(std::move(task));
  }

 private:
  Life life_;

  Owner* owner_;

  std::weak_ptr<Context> octx_;

  Path path_;

  std::weak_ptr<NodeRedirectContext> ictx_;
};


class SugarCall final : public File, public iface::Node {
 public:
  static inline TypeInfo kType = TypeInfo::New<SugarCall>(
      "Node/SugarCall", "sugar syntax of Node/Call",
      {typeid(iface::Memento), typeid(iface::Node)});

  SugarCall(Env* env, std::string_view path = "") noexcept :
      File(&kType, env), Node(Node::kMenu), memento_({this, path}) {
  }

  SugarCall(Env* env, const msgpack::object& obj)
  try : SugarCall(env, obj.as<std::string>()) {
  } catch (msgpack::type_error&) {
    throw DeserializeException("broken Node/SugarCall");
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack(memento_.data().path);
  }
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    return std::make_unique<SugarCall>(env, memento_.data().path);
  }

  void Update(RefStack&, const std::shared_ptr<Editor>& ctx) noexcept override {
    if (first_) {
      SyncSocks(ctx->basepath());
      memento_.Overwrite();
      first_ = false;
    }
  }
  void UpdateNode(RefStack&, const std::shared_ptr<Editor>&) noexcept override;
  void UpdateMenu(RefStack&, const std::shared_ptr<Editor>&) noexcept override;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Memento, iface::Node>(t).
        Select(this, &memento_);
  }

 private:
  struct UniversalData {
   public:
    UniversalData(SugarCall* owner, std::string_view p) noexcept :
        path(p), owner_(owner) {
    }
    void Restore(const UniversalData& other) noexcept {
      path      = other.path;
      in_socks  = other.in_socks;
      out_socks = other.out_socks;
      owner_->Rebuild(in_socks, out_socks);
    }

    // permanentized
    std::string path;

    // volatile
    std::vector<std::string> in_socks;
    std::vector<std::string> out_socks;

   private:
    SugarCall* owner_;
  };
  SimpleMemento<UniversalData> memento_;

  // volatile
  Life life_;

  std::string path_editing_;

  bool first_ = true;

  class CustomInSock;
  class CustomOutSock;
  std::vector<std::unique_ptr<CustomInSock>>  in_socks_;
  std::vector<std::unique_ptr<CustomOutSock>> out_socks_;


  Node* GetNode(const Path& basepath) const {
    const auto& udata = memento_.data();

    auto target = &*RefStack().Resolve(basepath).Resolve(udata.path);
    if (target == this) throw Exception("recursive reference");

    auto node = File::iface<iface::Node>(target);
    if (!node) throw Exception("target is not a Node");

    return node;
  }
  bool SyncSocks(const Path& basepath) noexcept
  try {
    auto& udata = memento_.data();
    auto  node  = GetNode(basepath);

    const auto in_names_bk  = std::move(udata.in_socks);
    const auto out_names_bk = std::move(udata.out_socks);

    auto  in       = node->in();
    auto& in_names = udata.in_socks;
    in_names.resize(in.size());
    in_socks_.resize(in.size());
    in_.resize(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
      in_names[i] = in[i]->name();
      in_socks_[i] = std::make_unique<CustomInSock>(this, in[i]->meta());
      in_[i] = &*in_socks_[i];
    }
    auto  out       = node->out();
    auto& out_names = udata.out_socks;
    out_names.resize(out.size());
    out_socks_.resize(out.size());
    out_.resize(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
      out_names[i] = out[i]->name();
      out_socks_[i] = std::make_unique<CustomOutSock>(this, out[i]->meta());
      out_[i] = &*out_socks_[i];
    }
    NotifySockChange();

    return in_names_bk != in_names || out_names_bk != out_names;
  } catch (Exception& e) {
    notify::Warn(basepath, this, e.msg());
    return false;
  }
  void Rebuild(std::span<const std::string> in_names,
               std::span<const std::string> out_names) noexcept {
    in_socks_.resize(in_names.size());
    in_.resize(in_names.size());
    for (size_t i = 0; i < in_names.size(); ++i) {
      in_socks_[i] = std::make_unique<CustomInSock>(this, in_names[i]);
      in_[i] = in_socks_[i].get();
    }
    out_socks_.resize(out_names.size());
    out_.resize(out_names.size());
    for (size_t i = 0; i < out_names.size(); ++i) {
      out_socks_[i] = std::make_unique<CustomOutSock>(this, out_names[i]);
      out_[i] = out_socks_[i].get();
    }
    NotifySockChange();
  }


  class InnerContext final : public Context, public Context::Data {
   public:
    InnerContext(SugarCall* owner, const std::shared_ptr<Context>& octx, Node* target) noexcept :
        Context(Path(octx->basepath())),
        owner_(owner), life_(owner->life_), octx_(octx), target_(target) {
    }

    void ObserveSend(const OutSock& sock, const Value& v) noexcept
    try {
      if (!*life_) throw Exception("received value in sub context but life is expired");
      if (sock.owner() != target_) return;

      auto dst = owner_->out(sock.name());
      if (!dst) throw Exception("missing output socket: "+sock.name());

      dst->Send(octx_, Value(v));
    } catch (Exception& e) {
      notify::Error(octx_->basepath(), owner_, e.msg());
    }

   private:
    SugarCall* owner_;

    Life::Ref life_;

    std::shared_ptr<Context> octx_;

    Node* target_;
  };
  class CustomInSock final : public InSock {
   public:
    CustomInSock(SugarCall* owner, const SockMeta& meta) noexcept :
        InSock(owner, &meta_), owner_(owner), meta_(meta) {
    }
    CustomInSock(SugarCall* owner, const std::string& name) noexcept :
        CustomInSock(owner, SockMeta {.name = name}) {
    }
    void Receive(const std::shared_ptr<Context>& ctx, Value&& v) noexcept override
    try {
      auto node = owner_->GetNode(ctx->basepath());

      auto sock = node->in(meta_.name);
      if (!sock) throw Exception("missing input socket: " + meta_.name);

      auto ictx = ctx->data<InnerContext>(owner_, owner_, ctx, node);
      sock->Receive(ictx, std::move(v));
    } catch (Exception& e) {
      notify::Error(ctx->basepath(), owner_, e.msg());
    }

   private:
    SugarCall* owner_;

    SockMeta meta_;
  };
  class CustomOutSock final : public OutSock {
   public:
    CustomOutSock(SugarCall* owner, const SockMeta& meta) noexcept :
        OutSock(owner, &meta_), meta_(meta) {
    }
    CustomOutSock(SugarCall* owner, const std::string& name) noexcept :
        CustomOutSock(owner, SockMeta {.name = name}) {
    }

   private:
    SockMeta meta_;
  };
};
void SugarCall::UpdateNode(RefStack& ref, const std::shared_ptr<Editor>& ctx) noexcept {
  const float em        = ImGui::GetFontSize();
  const float max_width = 8*em;

  auto& udata = memento_.data();

  ImGui::TextUnformatted("SUGAR CALL");

  const auto left = ImGui::GetCursorPosX();
  const auto top  = ImGui::GetCursorPosY();
  ImGui::AlignTextToFramePadding();
  ImGui::NewLine();

  ImGui::BeginGroup();
  {
    ImGui::BeginGroup();
    for (auto sock : in_) {
      gui::NodeInSock(sock->meta());
    }
    ImGui::EndGroup();

    float w = 0;
    for (auto sock : out_) {
      w = std::max(w, ImGui::CalcTextSize(sock->name().c_str()).x);
    }

    ImGui::SameLine();
    const auto x = ImGui::GetCursorPosX() - left;
    ImGui::Dummy({std::max(0.f, max_width-x-w), 1});
    ImGui::SameLine();

    ImGui::BeginGroup();
    for (auto sock : out_) {
      const float tw = ImGui::CalcTextSize(sock->name().c_str()).x;
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w-tw);
      gui::NodeOutSock(sock->meta());
    }
    ImGui::EndGroup();
  }
  ImGui::EndGroup();

  ImGui::SetCursorPosY(top);
  const float w = std::max(ImGui::GetItemRectSize().x, 8*em);
  ImGui::Button(udata.path.c_str(), {w, 0});
  if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
    UpdateMenu(ref, ctx);
    ImGui::EndPopup();
  }
}
void SugarCall::UpdateMenu(RefStack&, const std::shared_ptr<Editor>& ctx) noexcept {
  auto& udata = memento_.data();

  if (ImGui::MenuItem("sync socks")) {
    if (SyncSocks(ctx->basepath())) {
      memento_.Commit();
    }
  }

  ImGui::Separator();

  if (ImGui::BeginMenu("path")) {
    if (gui::InputPathMenu(ctx->basepath(), &path_editing_, &udata.path)) {
      if (SyncSocks(ctx->basepath())) {
        memento_.Commit();
      }
    }
    ImGui::EndMenu();
  }
}


class Cache final : public File, public iface::DirItem, public iface::Node {
 public:
  static inline TypeInfo kType = TypeInfo::New<Cache>(
      "Node/Cache", "stores execution result of Node",
      {typeid(iface::DirItem)});

  static inline const SockMeta kInParams = {
    .name = "params", .type = SockMeta::kNamedValue, .multi = true,
  };
  static inline const SockMeta kInExec = {
    .name = "exec", .type = SockMeta::kPulse, .trigger = true,
  };
  static inline const SockMeta kOutResult = {
    .name = "results", .type = SockMeta::kNamedValue, .multi = true,
  };

  Cache(Env* env, std::string_view path = "") noexcept :
      File(&kType, env),
      DirItem(DirItem::kMenu | DirItem::kTooltip),
      Node(Node::kNone),
      store_(new Store),
      out_result_(this, &kOutResult),
      in_params_(this, &kInParams,
                 [this](auto& ctx, auto&& v) { SetParam(ctx, std::move(v)); }),
      in_exec_(this, &kInExec, [this](auto& ctx, auto&&) { Exec(ctx); }),
      path_(path) {
    in_  = {&in_params_, &in_exec_};
    out_ = {&out_result_};
  }

  Cache(Env* env, const msgpack::object& obj)
  try : Cache(env, obj.as<std::string_view>()) {
  } catch (msgpack::type_error&) {
    throw DeserializeException("broken Node/Cache");
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack(path_);
  }
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    return std::make_unique<Cache>(env, path_);
  }

  void Update(RefStack& ref, Event&) noexcept override {
    basepath_ = ref.GetFullPath();
  }
  void UpdateMenu(RefStack&) noexcept override;
  void UpdateTooltip(RefStack&) noexcept override;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::DirItem, iface::Node>(t).Select(this);
  }

 private:
  class Store;
  std::shared_ptr<Store> store_;

  OutSock          out_result_;
  NodeLambdaInSock in_params_;
  NodeLambdaInSock in_exec_;

  // permanentized
  std::string path_;

  // volatile
  size_t try_cnt_ = 0;
  size_t hit_cnt_ = 0;

  std::string path_editing_;
  Path basepath_;
  bool last_error_ = false;


  void ClearStat() noexcept {
    try_cnt_ = 0, hit_cnt_ = 0;
    last_error_ = false;
  }
  Node* GetTargetNode(const RefStack& ref) {
    auto f = &*ref.Resolve(path_);
    if (f == this) throw Exception("self reference");

    auto n = File::iface<iface::Node>(f);
    if (!n) throw Exception("it's not a Node");
    return n;
  }

  void SetParam(const std::shared_ptr<Context>& ctx, Value&& v)
  try {
    auto cdata = ctx->data<ContextData>(this);
    auto& tup = v.tuple();
    cdata->params.emplace_back(tup[0].string(), tup[1]);
  } catch (Exception& e) {
    notify::Warn(basepath_, this, "error while taking parameter: "+e.msg());
  }
  void Exec(const std::shared_ptr<Context>& ctx) noexcept {
    ++try_cnt_;

    auto cdata  = ctx->data<ContextData>(this);
    auto params = std::move(cdata->params);

    // a lambda that passes target node's output to self output
    auto obs = [this, ctx](auto name, auto& value) {
      out_result_.Send(ctx, Value::Tuple { Value::String(name), Value(value) });
    };

    // observe the cache item when it's found
    if (auto item = store_->Find(cdata->params)) {
      ++hit_cnt_;
      item->Observe(std::move(obs));
      return;
    }

    // if the cache item is missing, create new one
    auto item = store_->Allocate(std::move(cdata->params));
    item->Observe(std::move(obs));

    // execute the target Node and store the result to the created item
    try {
      auto ref  = RefStack().Resolve(basepath_);
      auto n    = GetTargetNode(ref);
      auto ictx = std::make_shared<InnerContext>(Path(basepath_), ctx, n, item);
      for (const auto& p : item->in()) {
        const auto in = n->in();

        auto itr = std::find_if(in.begin(), in.end(),
                                [&m = p.first](auto& x) { return x->name() == m; });
        if (itr == in.end()) continue;
        (*itr)->Receive(ictx, Value(p.second));
      }
    } catch (Exception& e) {
      notify::Warn(basepath_, this, e.msg());
    }
  }


  using Param = std::pair<std::string, Value>;

  class StoreItem final {
   public:
    friend class Store;

    using Observer = std::function<void(std::string_view, const Value&)>;

    void Observe(Observer&& obs) noexcept {
      for (const auto& out : out_) {
        obs(out.first, out.second);
      }
      if (!finished_) obs_.push_back(obs);
    }
    void Set(std::string_view name, Value&& v) noexcept {
      assert(!finished_);

      out_.emplace_back(name, v);
      for (const auto& obs : obs_) {
        obs(name, v);
      }
    }

    // after calling this, Set() cannot be called
    void Finish() noexcept {
      obs_.clear();
      finished_ = true;
    }

    const std::vector<Param>& in() const noexcept { return in_; }
    const std::vector<Param>& out() const noexcept { return out_; }

   private:
    StoreItem() = delete;
    StoreItem(std::vector<Param>&& in) noexcept : in_(std::move(in)) {
    }

    std::vector<Param> in_, out_;

    std::vector<Observer> obs_;

    bool finished_ = false;
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

    InnerContext(Path&&                          basepath,
                 const std::shared_ptr<Context>& octx,
                 Node*                           target,
                 const std::weak_ptr<StoreItem>& item) noexcept :
        Context(std::move(basepath)), octx_(octx), target_(target), item_(item) {
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
        item->Set(std::move(name), std::move(v));
      };
      Queue::sub().Push(std::move(task));
    }

    std::vector<Node::InSock*> dstOf(const Node::OutSock* out) const noexcept override {
      return octx_->dstOf(out);
    }
    std::vector<Node::OutSock*> srcOf(const Node::InSock* in) const noexcept override {
      return octx_->srcOf(in);
    }

   private:
    std::shared_ptr<Node::Context> octx_;

    Node* target_;

    std::weak_ptr<StoreItem> item_;
  };

  class ContextData final : public Context::Data {
   public:
    std::vector<Param> params;
  };
};
void Cache::UpdateMenu(RefStack& ref) noexcept {
  if (ImGui::MenuItem("drop all cache")) {
    store_->DropAll();
  }
  if (ImGui::MenuItem("clear stat")) {
    ClearStat();
  }
  ImGui::Separator();
  if (ImGui::BeginMenu("target path")) {
    if (gui::InputPathMenu(ref, &path_editing_, &path_)) {
      store_->DropAll();
      ClearStat();
    }
    ImGui::EndMenu();
  }
}
void Cache::UpdateTooltip(RefStack&) noexcept {
  ImGui::Indent();
  ImGui::Text("target      : %s", path_.c_str());
  ImGui::Text("store size  : %zu", store_->size());
  ImGui::Text("try/hit/miss: %zu/%zu/%zu", try_cnt_, hit_cnt_, try_cnt_-hit_cnt_);
  if (try_cnt_ > 0) {
    ImGui::Text("hit rate    : %f%%",
                static_cast<float>(hit_cnt_)/static_cast<float>(try_cnt_)*100.f);
  }
  ImGui::Unindent();
}

} }  // namespace kingtaker
