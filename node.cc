#include "kingtaker.hh"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <iostream>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <ImNodes.h>
#include <linalg.hh>

#include "iface/dir.hh"
#include "iface/factory.hh"
#include "iface/history.hh"
#include "iface/node.hh"

#include "util/gui.hh"
#include "util/notify.hh"
#include "util/ptr_selector.hh"
#include "util/value.hh"

namespace kingtaker {
namespace {

class Network : public File, public iface::DirItem, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<Network>(
      "Node/Network", "manages multiple Nodes and connections between them",
      {typeid(iface::DirItem), typeid(iface::History)});

  static inline const std::string kIdSuffix = ": Network Editor";

  using IndexMap = std::unordered_map<Node*, size_t>;
  using NodeMap  = std::vector<Node*>;

  struct Conn final {
    std::weak_ptr<Node::InSock>  in;
    std::weak_ptr<Node::OutSock> out;
  };
  using ConnList = std::vector<Conn>;


  // wrapper struct for node file
  struct NodeHolder final {
   public:
    static std::unique_ptr<NodeHolder> Create(size_t id, std::unique_ptr<File>&& f) {
      auto n = File::iface<Node>(f.get());
      if (!n) return nullptr;
      return std::make_unique<NodeHolder>(id, std::move(f), n);
    }

    NodeHolder() = delete;
    NodeHolder(size_t id, std::unique_ptr<File>&& f, Node* n) noexcept :
        id_(id), file_(std::move(f)), entity_(n), first_(true) {
      assert(file_);
      assert(entity_);
    }
    NodeHolder(size_t                  id,
               std::unique_ptr<File>&& f,
               Node*                   n,
               ImVec2&&                p) noexcept :
        id_(id), file_(std::move(f)), entity_(n), pos_(std::move(p)) {
      assert(file_);
      assert(entity_);
    }

    static std::unique_ptr<NodeHolder> Deserialize(
        const msgpack::object&, const std::shared_ptr<Env>&);
    void DeserializeLink(const msgpack::object&, const NodeMap&);
    void Serialize(Packer&) const noexcept;
    void SerializeLink(Packer& pk, const IndexMap& idxmap) const noexcept;

    std::unique_ptr<NodeHolder> Clone(size_t id, const std::shared_ptr<Env>& env) const noexcept {
      return NodeHolder::Create(id, file_->Clone(env));
    }

    void Setup(Network* owner) noexcept {
      auto inter = dynamic_cast<InternalNode*>(file_.get());
      if (inter) inter->Setup(owner);
    }
    void Teardown(Network* owner) noexcept {
      auto inter = dynamic_cast<InternalNode*>(file_.get());
      if (inter) inter->Teardown(owner);
    }

    void UpdateNode(Network*, RefStack&) noexcept;
    void UpdateWindow(RefStack&, Event&) noexcept;

    void Select() noexcept { select_ = true; }
    void Unselect() noexcept { select_ = false; }

    size_t id() const noexcept { return id_; }
    File& file() const noexcept { return *file_; }
    Node& entity() const noexcept { return *entity_; }

    const ImVec2& pos() const noexcept { return pos_; }

   private:
    // permanentized
    size_t id_;

    std::unique_ptr<File> file_;
    Node*                 entity_;

    ImVec2 pos_    = {0, 0};
    bool   select_ = false;

    // volatile
    bool first_ = false;
  };
  using NodeHolderList    = std::vector<std::unique_ptr<NodeHolder>>;
  using NodeHolderRefList = std::vector<NodeHolder*>;


  Network(const std::shared_ptr<Env>& env,
          Time lastmod = Clock::now(),
          NodeHolderList&& nodes = {},
          size_t next = 0) noexcept :
      File(&type_, env), DirItem(DirItem::kMenu), Node(Node::kNone),
      ctx_(std::make_shared<Context>()),
      lastmod_(lastmod), nodes_(std::move(nodes)), next_id_(next),
      history_(this) {
    canvas_.Style.NodeRounding = 0.f;
  }

  static std::unique_ptr<File> Deserialize(
      const msgpack::object&, const std::shared_ptr<Env>&);
  void Serialize(Packer&) const noexcept override;

  void DeserializeGUI(const msgpack::object&) noexcept;
  void SerializeGUI(Packer&) const noexcept;

  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override;

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
  void Focus(RefStack& ref, NodeHolder* target) noexcept {
    for (auto& h : nodes_) h->Unselect();
    target->Select();

    // adjust offset to make the node displayed in center
    canvas_.Offset = (target->pos()*canvas_.Zoom - canvas_size_/2.f)*-1.f;

    // focus the editor
    const auto id = ref.Stringify() + kIdSuffix;
    ImGui::SetWindowFocus(id.c_str());
    shown_ = true;
  }

  void Update(RefStack& ref, Event& ev) noexcept override;
  void UpdateMenu(RefStack&) noexcept;
  void UpdateCanvas(RefStack&) noexcept;
  template <typename T, typename U>
  void UpdateNewIO(std::vector<std::shared_ptr<U>>& list) noexcept;
  void Update(RefStack&, const std::shared_ptr<Context>&) noexcept override;

  Time lastModified() const noexcept override {
    return lastmod_;
  }
  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::DirItem, iface::History, iface::Node>(t).
        Select(this, &history_);
  }

 private:
  std::shared_ptr<Context> ctx_;
  ImNodes::CanvasState canvas_;

  // permanentized params
  Time           lastmod_;
  NodeHolderList nodes_;
  size_t         next_id_ = 0;

  bool shown_ = false;

  // volatile params
  std::string io_new_name_;

  ImVec2 canvas_size_;


  // MSVC doesn't allow inner class to access protected members came from super class.
  auto& node_in_() noexcept { return in_; }
  auto& node_out_() noexcept { return out_; }

  NodeHolder& FindHolder(const Node& n) const noexcept {
    for (auto& h: nodes_) {
      if (&h->entity() == &n) return *h;
    }
    assert(false);
    std::abort();
  }
  NodeHolder* FindHolder(size_t id) const noexcept {
    for (auto& h: nodes_) {
      if (h->id() == id) return h.get();
    }
    return nullptr;
  }
  NodeHolder* FindHolder(const std::string& name) const noexcept {
    size_t pos;
    const auto id = static_cast<size_t>(std::stoll(std::string(name), &pos));
    return name.size() == pos? FindHolder(id): nullptr;
  }


  // History interface implementation
  class History : public iface::SimpleHistory<> {
   public:
    History(Network* o) : owner_(o) { }

    void AddNodeIf(std::unique_ptr<NodeHolder>&& h) noexcept {
      if (!h) return;

      NodeHolderList list;
      list.push_back(std::move(h));
      AddNodes(std::move(list));
    }
    void AddNodes(NodeHolderList&& h) noexcept;
    void RemoveNodes(NodeHolderRefList&& h) noexcept;
    void Link(ConnList&& conns) noexcept;
    void Unlink(ConnList&& conns) noexcept;

   private:
    Network* owner_;

    class LinkSwapCommand;
    class SwapCommand;
  } history_;


  // an interface for nodes depends on Network
  class InternalNode {
   public:
    InternalNode() = default;
    virtual ~InternalNode() = default;

    // called when this is inserted to the Network
    virtual void Setup(Network*) noexcept { }

    // called when this is removed from the Network
    virtual void Teardown(Network*) noexcept { }
  };


  // common implementation for IO socks of Network
  // T must be InSock or OutSock
  template <typename T>
  class AbstractIONode : public File, public Node, public InternalNode {
   public:
    // sock interface for Network IO
    class SharedSock : public T {
     public:
      SharedSock(Network* owner, std::string_view name) noexcept : T(owner, name) {
      }

      // called when new AbstractIONode which has the same name is added
      virtual void Attach(AbstractIONode* n) noexcept = 0;

      // called when AbstractIONode which has the same name is deleted
      // returning false can remove this socket from Network
      virtual bool Detach(AbstractIONode* n) noexcept = 0;
    };

    AbstractIONode(TypeInfo* t, const std::shared_ptr<Env>& env, std::string_view name) :
        File(t, env), Node(kNone), name_(name) {
    }

    // attaches this node to the socket which has the same name,
    // or adds new one to Network if there's no such socket
    void Setup(Network* owner) noexcept override {
      owner_ = owner;

      auto& list = GetList(owner);
      auto  itr  = std::find_if(
          list.begin(), list.end(), [this](auto& e) { return e->name() == name_; });
      if (itr != list.end()) {
        ctx_sock_ = std::dynamic_pointer_cast<SharedSock>(*itr);
      } else {
        ctx_sock_ = CreateSock(owner, name_);

        list.push_back(ctx_sock_);
        std::sort(list.begin(), list.end(), [](auto& a, auto& b) {
                    return a->name() < b->name();
                  });
      }
      assert(ctx_sock_);
      ctx_sock_->Attach(this);
    }
    // detaches this node from the socket which has the same name
    // removes the socket if it's requested
    void Teardown(Network* owner) noexcept override {
      auto& list = GetList(owner);
      if (ctx_sock_->Detach(this)) {
        auto itr = std::find(list.begin(), list.end(), ctx_sock_);
        if (itr != list.end()) list.erase(itr);
      }
      owner_    = nullptr;
      ctx_sock_ = nullptr;
    }

    void* iface(const std::type_index& t) noexcept override {
      return PtrSelector<iface::Node>(t).Select(this);
    }

   protected:
    std::string name_;

    Network* owner_ = nullptr;

    std::shared_ptr<SharedSock> ctx_sock_;

    using Node::in_;
    using Node::out_;

    // returns a reference of target socket list from Network
    virtual std::vector<std::shared_ptr<T>>& GetList(Network*) const noexcept = 0;

    // creates new socket for Network
    virtual std::shared_ptr<SharedSock> CreateSock(Network*, std::string_view) const noexcept = 0;
  };

  // A Node that emits input provided to Network
  class InputNode : public AbstractIONode<InSock> {
   public:
    static inline TypeInfo type_ = TypeInfo::New<InputNode>(
        "Node/Network/Input", "input emitter in Node/Network", {});

    InputNode(const std::shared_ptr<Env>& env, std::string_view name) noexcept :
        AbstractIONode(&type_, env, name) {
      out_.emplace_back(new OutSock(this, "out"));
    }

    static std::unique_ptr<File> Deserialize(const msgpack::object& obj, const std::shared_ptr<Env>& env) {
      return std::make_unique<InputNode>(env, obj.as<std::string>());
    }
    void Serialize(Packer& pk) const noexcept override {
      pk.pack(name_);
    }
    std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
      return std::make_unique<InputNode>(env, name_);
    }

    void Update(RefStack&, const std::shared_ptr<Context>&) noexcept override;

   private:
    std::vector<std::shared_ptr<InSock>>& GetList(Network* owner) const noexcept override {
      return owner->node_in_();
    }
    std::shared_ptr<SharedSock> CreateSock(Network* o, std::string_view n) const noexcept override {
      class Sock final : public SharedSock {
       public:
        Sock(Network* o, std::string_view n) : SharedSock(o, n) { }
        void Attach(AbstractIONode* n) noexcept override {
          n_.insert(n);
        }
        bool Detach(AbstractIONode* n) noexcept override {
          n_.erase(n); return n_.empty();
        }
        void Receive(const std::shared_ptr<Context>& ctx, Value&& v) noexcept override {
          for (auto n : n_) n->out()[0]->Send(ctx, Value(v));
        }
       private:
        std::unordered_set<AbstractIONode*> n_;
      };
      return std::make_shared<Sock>(o, n);
    }
  };

  // A Node that receives an output for Network
  class OutputNode : public AbstractIONode<OutSock> {
   public:
    static inline TypeInfo type_ = TypeInfo::New<OutputNode>(
        "Node/Network/Output", "output receiver in Node/Network", {});

    OutputNode(const std::shared_ptr<Env>& env, std::string_view name) noexcept :
        AbstractIONode(&type_, env, name), life_(std::make_shared<std::monostate>()) {
      std::weak_ptr<std::monostate> wlife = life_;
      auto handler = [this, wlife, name = name_](const auto& ctx, auto&& v) {
        if (wlife.expired() || !owner_) return;

        auto sock = owner_->FindOut(name_);
        if (!sock) return;

        sock->Send(ctx, std::move(v));
      };
      in_.emplace_back(new LambdaInSock(this, "in", std::move(handler)));
    }

    static std::unique_ptr<File> Deserialize(const msgpack::object& obj, const std::shared_ptr<Env>& env) {
      return std::make_unique<OutputNode>(env, obj.as<std::string>());
    }
    void Serialize(Packer& pk) const noexcept override {
      pk.pack(name_);
    }
    std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
      return std::make_unique<OutputNode>(env, name_);
    }

    void Update(RefStack&, const std::shared_ptr<Context>&) noexcept override;

   private:
    std::shared_ptr<std::monostate> life_;

    std::vector<std::shared_ptr<OutSock>>& GetList(Network* owner) const noexcept override {
      return owner->node_out_();
    }
    std::shared_ptr<SharedSock> CreateSock(Network* o, std::string_view n) const noexcept override {
      class Sock final : public SharedSock {
       public:
        Sock(Network* o, std::string_view n) : SharedSock(o, n) {
        }
        void Attach(AbstractIONode*) noexcept override { ++refcnt_; }
        bool Detach(AbstractIONode*) noexcept override { return !--refcnt_; }
       private:
        size_t refcnt_ = 0;
      };
      return std::make_shared<Sock>(o, n);
    }
  };
};

std::unique_ptr<File> Network::Deserialize(
    const msgpack::object& obj, const std::shared_ptr<Env>& env) {
  try {
    const auto lastmod = msgpack::find(obj, "lastmod"s).as<Time>();

    const auto next = msgpack::find(obj, "nextId"s).as<size_t>();

    auto& obj_nodes = msgpack::find(obj, "nodes"s);
    if (obj_nodes.type != msgpack::type::ARRAY) throw msgpack::type_error();

    NodeMap nmap(obj_nodes.via.array.size);
    NodeHolderList nodes(obj_nodes.via.array.size);

    for (size_t i = 0; i < obj_nodes.via.array.size; ++i) {
      nodes[i] = NodeHolder::Deserialize(obj_nodes.via.array.ptr[i], env);
      nmap[i]  = &nodes[i]->entity();

      if (nodes[i]->id() == next) {
        throw DeserializeException("nodeId conflict");
      }
    }

    auto& obj_links = msgpack::find(obj, "links"s);
    if (obj_links.type != msgpack::type::ARRAY) throw msgpack::type_error();
    if (obj_links.via.array.size > nmap.size()) {
      throw DeserializeException("broken Network");
    }
    for (size_t i = 0; i < obj_links.via.array.size; ++i) {
      nodes[i]->DeserializeLink(obj_links.via.array.ptr[i], nmap);
    }

    auto ret = std::make_unique<Network>(env, lastmod, std::move(nodes), next);
    ret->DeserializeGUI(msgpack::find(obj, "gui"s));
    for (auto& h : ret->nodes_) h->Setup(ret.get());
    return ret;

  } catch (msgpack::type_error&) {
    throw DeserializeException("broken data structure");
  }
}
void Network::Serialize(Packer& pk) const noexcept {
  std::unordered_map<Node*, size_t> idxmap;

  pk.pack_map(5);

  pk.pack("lastmod"s);
  pk.pack(lastmod_);

  pk.pack("nodes"s);
  pk.pack_array(static_cast<uint32_t>(nodes_.size()));
  for (size_t i = 0; i < nodes_.size(); ++i) {
    auto& h = nodes_[i];
    h->Serialize(pk);
    idxmap[&h->entity()] = i;
  }

  pk.pack("links"s);
  pk.pack_array(static_cast<uint32_t>(nodes_.size()));
  for (auto& h : nodes_) {
    h->SerializeLink(pk, idxmap);
  }

  pk.pack("nextId"s);
  pk.pack(next_id_);

  pk.pack("gui"s);
  SerializeGUI(pk);
}

void Network::DeserializeGUI(const msgpack::object& obj) noexcept {
  try {
    shown_ = msgpack::find(obj, "shown"s).as<bool>();

    canvas_.Zoom = msgpack::find(obj, "zoom"s).as<float>();

    std::pair<float, float> offset;
    msgpack::find(obj, "offset"s).convert(offset);
    canvas_.Offset = {offset.first, offset.second};

  } catch (msgpack::type_error&) {
    shown_         = false;
    canvas_.Zoom   = 1.f;
    canvas_.Offset = {0, 0};
  }
}
void Network::SerializeGUI(Packer& pk) const noexcept {
  pk.pack_map(3);

  pk.pack("shown"s);
  pk.pack(shown_);

  pk.pack("zoom"s);
  pk.pack(canvas_.Zoom);

  pk.pack("offset"s);
  pk.pack(std::make_tuple(canvas_.Offset.x, canvas_.Offset.y));
}

std::unique_ptr<Network::NodeHolder> Network::NodeHolder::Deserialize(
    const msgpack::object& obj, const std::shared_ptr<Env>& env) {
  try {
    const size_t id = msgpack::find(obj, "id"s).as<size_t>();

    auto f = File::Deserialize(msgpack::find(obj, "file"s), env);
    auto n = File::iface<Node>(f.get());
    if (!n) throw DeserializeException("it's not node");

    std::pair<float, float> p;
    msgpack::find(obj, "pos"s).convert(p);

    auto h = std::make_unique<NodeHolder>(
        id, std::move(f), n, ImVec2 {p.first, p.second});
    try {
      h->select_ = msgpack::find(obj, "select"s).as<bool>();
    } catch (msgpack::type_error&) {
    }
    return h;

  } catch (msgpack::type_error&) {
    throw DeserializeException("broken NodeHolder structure");
  }
}
void Network::NodeHolder::Serialize(Packer& pk) const noexcept {
  pk.pack_map(4);

  pk.pack("id"s);
  pk.pack(id_);

  pk.pack("file"s);
  file_->SerializeWithTypeInfo(pk);

  pk.pack("pos"s);
  pk.pack(std::make_pair(pos_.x, pos_.y));

  pk.pack("select"s);
  pk.pack(select_);
}

void Network::NodeHolder::DeserializeLink(const msgpack::object& obj, const NodeMap& nmap) {
  try {
    if (obj.type != msgpack::type::MAP) throw msgpack::type_error();
    for (size_t i = 0; i < obj.via.map.size; ++i) {
      auto  out_name = obj.via.map.ptr[i].key.as<std::string>();
      auto& out_dst  = obj.via.map.ptr[i].val;

      auto out = entity_->FindOut(out_name);
      if (!out) continue;

      for (size_t j = 0; j < out_dst.via.array.size; ++j) {
        std::pair<size_t, std::string> conn;
        if (!out_dst.via.array.ptr[j].convert_if_not_nil(conn)) {
          continue;
        }
        if (conn.first >= nmap.size()) throw DeserializeException("missing node");

        auto in = nmap[conn.first]->FindIn(conn.second);
        if (!in) continue;

        Node::Link(out, in);
      }
    }
  } catch (msgpack::type_error&) {
    throw DeserializeException("broken NodeHolder link");
  }
}
void Network::NodeHolder::SerializeLink(Packer& pk, const IndexMap& idxmap) const noexcept {
  pk.pack_map(static_cast<uint32_t>(entity_->out().size()));
  for (auto out : entity_->out()) {
    out->CleanConns();
    std::vector<std::shared_ptr<InSock>> socks;
    for (auto& inw : out->dst()) {
      auto in = inw.lock();
      if (in) socks.push_back(in);
    }

    pk.pack(out->name());
    pk.pack_array(static_cast<uint32_t>(socks.size()));
    for (auto& in : socks) {
      auto itr = idxmap.find(&in->owner());
      if (itr != idxmap.end()) {
        pk.pack(std::make_tuple(itr->second, in->name()));
      } else {
        pk.pack_nil();
      }
    }
  }
}

std::unique_ptr<File> Network::Clone(const std::shared_ptr<Env>& env) const noexcept {
  std::unordered_map<Node*, Node*> nmap;

  size_t id = 0;

  NodeHolderList nodes;
  nodes.reserve(nodes_.size());
  for (auto& h : nodes_) {
    nodes.push_back(h->Clone(id++, env));
    nmap[&h->entity()] = &nodes.back()->entity();
  }
  for (auto& h : nodes) {
    auto srcn = &h->entity();
    auto dstn = nmap[srcn];

    for (auto srcoutsock : srcn->out()) {
      auto dstoutsock = dstn->FindOut(srcoutsock->name());
      if (!dstoutsock) continue;

      for (auto& w_srcinsock : srcoutsock->dst()) {
        auto srcinsock = w_srcinsock.lock();
        auto dstinsock = nmap[&srcinsock->owner()]->FindIn(srcinsock->name());
        if (!dstinsock) continue;

        Node::Link(dstoutsock, dstinsock);
      }
    }
  }
  return std::make_unique<Network>(env, Clock::now(), std::move(nodes));
}

void Network::Update(RefStack& ref, Event& ev) noexcept {
  for (auto& h : nodes_) {
    if (ev.IsFocused(&h->file())) Focus(ref, h.get());
    h->UpdateWindow(ref, ev);
  }

  const auto id = ref.Stringify() + kIdSuffix;
  if (ev.IsFocused(this)) {
    ImGui::SetWindowFocus(id.c_str());
    shown_ = true;
  }

  const auto size = ImVec2(24.f, 24.f)*ImGui::GetFontSize();
  ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);

  constexpr auto kFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
  if (ImGui::Begin(id.c_str(), &shown_, kFlags)) {
    canvas_size_ = ImGui::GetWindowSize();
    UpdateCanvas(ref);
  }
  ImGui::End();
}
void Network::UpdateMenu(RefStack&) noexcept {
  ImGui::MenuItem("Network Editor", nullptr, &shown_);
}
void Network::UpdateCanvas(RefStack& ref) noexcept {
  ImNodes::BeginCanvas(&canvas_);
  gui::NodeCanvasSetZoom();

  gui::NodeCanvasResetZoom();
  if (ImGui::BeginPopupContextItem()) {
    if (ImGui::BeginMenu("New")) {
      for (auto& p : File::registry()) {
        auto& t = *p.second;
        if (!t.factory() || !t.CheckImplemented<Node>()) continue;
        if (ImGui::MenuItem(t.name().c_str())) {
          history_.AddNodeIf(NodeHolder::Create(next_id_++, t.Create(env())));
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("%s", t.desc().c_str());
        }
      }
      ImGui::Separator();
      if (ImGui::BeginMenu("Input")) {
        UpdateNewIO<InputNode>(in_);
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Output")) {
        UpdateNewIO<OutputNode>(out_);
        ImGui::EndMenu();
      }
      ImGui::EndMenu();
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Undo")) {
      history_.Move(-1);
    }
    if (ImGui::MenuItem("Redo")) {
      history_.Move(1);
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Clear history")) {
      history_.Clear();
    }
    if (ImGui::MenuItem("Clear entire context")) {
      Queue::main().Push([this]() { ctx_ = std::make_shared<Context>(); });
    }
    ImGui::EndPopup();
  }
  gui::NodeCanvasSetZoom();

  for (auto& h : nodes_) {
    h->UpdateNode(this, ref);
  }

  ConnList rm_conns;
  for (auto& h : nodes_) {
    auto node = &h->entity();
    for (auto src : node->out()) {
      for (auto w_dst : src->dst()) {
        auto dst = w_dst.lock();
        if (!dst) continue;

        auto& srch = FindHolder(src->owner());
        auto  srcs = src->name().c_str();
        auto& dsth = FindHolder(dst->owner());
        auto  dsts = dst->name().c_str();
        if (!ImNodes::Connection(&dsth, dsts, &srch, srcs)) {
          rm_conns.push_back({dst, src});
        }
      }
    }
  }
  if (rm_conns.size()) {
    history_.Unlink(std::move(rm_conns));
  }

  void* inptr;
  void* outptr;
  const char* srcs;
  const char* dsts;
  if (ImNodes::GetNewConnection(&inptr, &dsts, &outptr, &srcs)) {
    auto dstn = reinterpret_cast<NodeHolder*>(inptr);
    auto srcn = reinterpret_cast<NodeHolder*>(outptr);

    auto src = srcn->entity().FindOut(srcs);
    auto dst = dstn->entity().FindIn(dsts);
    if (src && dst) history_.Link({{dst, src}});
  }

  gui::NodeCanvasResetZoom();
  ImNodes::EndCanvas();
}
template <typename T, typename U>
void Network::UpdateNewIO(std::vector<std::shared_ptr<U>>& list) noexcept {
  constexpr auto kFlags =
      ImGuiInputTextFlags_EnterReturnsTrue |
      ImGuiInputTextFlags_AutoSelectAll;

  static const char* kHint = "enter to add...";

  ImGui::SetKeyboardFocusHere();
  const bool submit =
      ImGui::InputTextWithHint("##newIO", kHint, &io_new_name_, kFlags);

  const bool empty = io_new_name_.empty();
  const bool dup   = list.end() !=
      std::find_if(list.begin(), list.end(),
                   [this](auto& e) { return e->name() == io_new_name_; });

  if (empty) {
    ImGui::Bullet();
    ImGui::TextUnformatted("empty name");
  }
  if (dup) {
    ImGui::Bullet();
    ImGui::TextUnformatted("name duplication");
  }
  if (submit && !empty && !dup) {
    history_.AddNodeIf(NodeHolder::Create(
            next_id_++, std::make_unique<T>(env(), io_new_name_)));
    io_new_name_ = "";
    ImGui::CloseCurrentPopup();
  }
}
void Network::NodeHolder::UpdateWindow(RefStack& ref, Event& ev) noexcept {
  ref.Push({std::to_string(id_), file_.get()});
  ImGui::PushID(file_.get());

  file_->Update(ref, ev);

  ImGui::PopID();
  ref.Pop();
}
void Network::NodeHolder::UpdateNode(Network* owner, RefStack& ref) noexcept {
  ref.Push({std::to_string(id_), file_.get()});
  ImGui::PushID(file_.get());

  if (first_) {
    ImNodes::AutoPositionNode(this);
    first_ = false;
  }

  if (ImNodes::BeginNode(this, &pos_, &select_)) {
    entity_->Update(ref, owner->ctx_);
  }
  ImNodes::EndNode();

  gui::NodeCanvasResetZoom();
  if (ImGui::BeginPopupContextItem()) {
    if (ImGui::MenuItem("Clone")) {
      owner->history_.AddNodeIf(Clone(owner->next_id_++, owner->env()));
    }
    if (ImGui::MenuItem("Remove")) {
      owner->history_.RemoveNodes({this});
    }
    if (entity_->flags() & Node::kMenu) {
      ImGui::Separator();
      entity_->UpdateMenu(ref, owner->ctx_);
    }
    ImGui::EndPopup();
  }
  gui::NodeCanvasSetZoom();

  ImGui::PopID();
  ref.Pop();
}

void Network::Update(RefStack&, const std::shared_ptr<Context>&) noexcept {
  const auto em   = ImGui::GetFontSize();
  const auto line = ImGui::GetCursorPosY();
  ImGui::NewLine();

  ImGui::BeginGroup();
  if (in_.size() || out_.size()) {
    ImGui::BeginGroup();
    for (auto& in : in_) {
      const auto c = in->name().c_str();
      if (ImNodes::BeginInputSlot(c, 1)) {
        gui::NodeSocket();
        ImNodes::EndSlot();
      }
      ImGui::SameLine();
      ImGui::TextUnformatted(c);
    }
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::Dummy({2*em, 1});
    ImGui::SameLine();

    float wmax = 0;
    for (auto& out : out_) {
      wmax = std::max(wmax, ImGui::CalcTextSize(out->name().c_str()).x);
    }
    ImGui::BeginGroup();
    for (auto& out : out_) {
      const auto c = out->name().c_str();
      const auto w = ImGui::CalcTextSize(c).x;

      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (wmax-w));
      ImGui::TextUnformatted(c);

      ImGui::SameLine();
      if (ImNodes::BeginOutputSlot(c, 1)) {
        gui::NodeSocket();
        ImNodes::EndSlot();
      }
    }
    ImGui::EndGroup();
  } else {
    ImGui::TextDisabled("No I/O");
  }
  ImGui::EndGroup();

  static const char* title = "Network";
  const auto w  = ImGui::GetItemRectSize().x;
  const auto tw = ImGui::CalcTextSize(title).x;
  if (w > tw) {
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w-tw)/2);
  }
  ImGui::SetCursorPosY(line);
  ImGui::TextUnformatted("Network");
}
void Network::InputNode::Update(RefStack& ref, const std::shared_ptr<Context>&) noexcept {
  auto owner = ref.FindParent<Network>();
  if (!owner) {
    ImGui::TextUnformatted("INPUT");
    ImGui::TextUnformatted("ERROR X(");
    ImGui::TextUnformatted("This node must be used at inside of Network");
    return;
  }

  ImGui::Text("IN> %s", ctx_sock_->name().c_str());

  ImGui::SameLine();
  if (ImNodes::BeginOutputSlot("out", 1)) {
    gui::NodeSocket();
    ImNodes::EndSlot();
  }
}
void Network::OutputNode::Update(RefStack& ref, const std::shared_ptr<Context>&) noexcept {
  auto owner = ref.FindParent<Network>();
  if (!owner) {
    ImGui::TextUnformatted("OUTPUT");
    ImGui::TextUnformatted("ERROR X(");
    ImGui::TextUnformatted("This node must be used at inside of Network");
    return;
  }

  if (ImNodes::BeginInputSlot("in", 1)) {
    gui::NodeSocket();
    ImNodes::EndSlot();
  }

  ImGui::SameLine();
  ImGui::Text("%s >OUT", ctx_sock_->name().c_str());
}

// a command for link creation or removal
class Network::History::LinkSwapCommand : public Command {
 public:
  enum Type { kLink, kUnlink, };

  LinkSwapCommand(Type t, ConnList&& conns) noexcept :
      type_(t), conns_(std::move(conns)) {
  }

  void Link() const {
    for (auto& conn : conns_) {
      if (!Node::Link(conn.out, conn.in)) {
        throw Exception("cannot link deleted socket");
      }
    }
  }
  void Unlink() const {
    for (auto& conn : conns_) {
      if (!Node::Unlink(conn.out, conn.in)) {
        throw Exception("cannot unlink deleted socket");
      }
    }
  }
  void Apply() override {
    switch (type_) {
    case kLink  : Link();   break;
    case kUnlink: Unlink(); break;
    }
  }
  void Revert() override {
    switch (type_) {
    case kLink  : Unlink(); break;
    case kUnlink: Link();   break;
    }
  }

 private:
  Type     type_;
  ConnList conns_;
};
void Network::History::Link(ConnList&& conns) noexcept {
  Queue(std::make_unique<LinkSwapCommand>(LinkSwapCommand::kLink, std::move(conns)));
}
void Network::History::Unlink(ConnList&& conns) noexcept {
  Queue(std::make_unique<LinkSwapCommand>(LinkSwapCommand::kUnlink, std::move(conns)));
}

// a command for node creation or removal
class Network::History::SwapCommand : public Command {
 public:
  SwapCommand(Network* o, NodeHolderList&& h = {}) :
      owner_(o), holders_(std::move(h)) {
    refs_.reserve(holders_.size());
    for (auto& holder : holders_) refs_.push_back(holder.get());
  }
  SwapCommand(Network* o, NodeHolderRefList&& refs = {}) :
      owner_(o), refs_(std::move(refs)) {
  }

  void Exec() {
    auto& nodes = owner_->nodes_;
    if (holders_.size()) {
      for (auto& h : holders_) {
        h->Setup(owner_);
        nodes.push_back(std::move(h));
      }
      holders_.clear();

      if (links_) links_->Link();
    } else {
      SaveLinks();
      links_->Unlink();

      holders_.reserve(refs_.size());
      for (auto& r : refs_) {
        auto itr = std::find_if(nodes.begin(), nodes.end(),
                                [r](auto& e) { return e.get() == r; });
        if (itr == nodes.end()) {
          throw Exception("target node is missing");
        }
        r->Teardown(owner_);
        holders_.push_back(std::move(*itr));
        nodes.erase(itr);
      }
    }
  }
  void Apply() override { Exec(); }
  void Revert() override { Exec(); }

 private:
  void SaveLinks() {
    ConnList conns;
    for (auto h : refs_) {
      auto& n = h->entity();
      for (auto& out : n.out()) {
        for (auto& w_in : out->dst()) {
          auto in = w_in.lock();
          if (!in) continue;
          conns.push_back({in, out});
        }
      }
      for (auto& in : n.in()) {
        for (auto& w_out : in->src()) {
          auto out = w_out.lock();
          if (!out) continue;

          auto found = std::find_if(
              refs_.begin(), refs_.end(),
              [&out](auto& e) { return &e->entity() == &out->owner(); });
          if (found != refs_.end()) continue;
          conns.push_back({in, out});
        }
      }
    }
    links_.emplace(LinkSwapCommand::kUnlink, std::move(conns));
  }

  Network*                       owner_;
  NodeHolderList                 holders_;
  NodeHolderRefList              refs_;
  std::optional<LinkSwapCommand> links_;
};
void Network::History::AddNodes(NodeHolderList&& h) noexcept {
  Queue(std::make_unique<SwapCommand>(owner_, std::move(h)));
}
void Network::History::RemoveNodes(NodeHolderRefList&& h) noexcept {
  Queue(std::make_unique<SwapCommand>(owner_, std::move(h)));
}


class Ref : public File, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<Ref>(
      "Node/Ref", "allows other nodes to be treated as lambda",
      {typeid(iface::Node)});

  using Life = std::weak_ptr<std::monostate>;

  Ref(const std::shared_ptr<Env>& env,
          std::string_view path = "",
          const std::vector<std::string>& in  = {},
          const std::vector<std::string>& out = {}) noexcept :
      File(&type_, env), Node(kMenu),
      path_(path),
      life_(std::make_shared<std::monostate>()),
      ctx_(std::make_shared<Context>()) {
    in_.reserve(in.size());
    out_.reserve(out.size());

    const auto p = ParsePath(path);
    for (auto& v : in) in_.push_back(std::make_shared<Input>(this, v));
    for (auto& v : out) out_.push_back(std::make_shared<OutSock>(this, v));
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    auto str = msgpack::find(obj, "path"s).as<std::string>();
    auto in  = msgpack::find(obj, "in"s).as<std::vector<std::string>>();
    auto out = msgpack::find(obj, "out"s).as<std::vector<std::string>>();
    return std::make_unique<Ref>(env, str, std::move(in), std::move(out));
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_map(3);

    pk.pack("in"s);
    pk.pack_array(static_cast<uint32_t>(in_.size()));
    for (auto& e : in_) pk.pack(e->name());

    pk.pack("out"s);
    pk.pack_array(static_cast<uint32_t>(out_.size()));
    for (auto& e : out_) pk.pack(e->name());

    pk.pack("path"s);
    pk.pack(path_);
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<Ref>(env, path_);
  }

  void Update(RefStack& ref, Event&) noexcept override {
    basepath_ = ref.GetFullPath();
    Watch(&*ref.Resolve(path_));
  }
  void Update(RefStack&, const std::shared_ptr<Context>&) noexcept override;
  void UpdateMenu(RefStack&, const std::shared_ptr<Context>& ctx) noexcept override;
  void UpdateEntity(RefStack&, File* f) noexcept;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node>(t).Select(this);
  }

 private:
  // permanentized params
  std::string path_;

  // volatile params
  std::shared_ptr<std::monostate> life_;
  std::shared_ptr<Context>        ctx_;
  File::Path                      basepath_;

  bool        synced_ = false;
  std::string path_editing_;
  Time        lastmod_;


  // detects changes of the entity
  void Watch(File* f) noexcept {
    if (f == this) return;
    if (!synced_) SyncSocks(f);

    auto node = File::iface<Node>(f);
    if (node) return;

    auto factory = File::iface<iface::Factory<Value>>(f);
    if (factory) {
      const auto mod = f->lastModified();
      if (lastmod_ < mod) {
        out_[0]->Send(ctx_, factory->Create());
        lastmod_ = mod;
      }
      return;
    }
  }

  // synchronize IO socket list
  // T must be a InSock or OutSock
  template <typename T>
  static void Sync(std::vector<std::shared_ptr<T>>& dst,
                   const std::span<const std::shared_ptr<T>>& src,
                   std::function<std::shared_ptr<T>(std::string_view)>&& f) noexcept {
    std::unordered_map<std::string, std::shared_ptr<T>> m;
    for (auto& e : dst) m[e->name()] = e;

    dst.clear();
    for (auto& e : src) {
      auto itr = m.find(e->name());
      if (itr != m.end()) {
        dst.push_back(itr->second);
      } else {
        dst.push_back(f(e->name()));
      }
    }
  }
  void SyncSocks(File* f) noexcept {
    synced_ = true;

    // self reference is illegal
    if (f == this) {
      in_.clear();
      out_.clear();
      return;
    }

    auto node = File::iface<Node>(f);
    if (node) {
      Sync<InSock>(in_, node->in(), [this](auto str) {
             return std::make_shared<Input>(this, str);
           });
      Sync<OutSock>(out_, node->out(), [this](auto str) {
             return std::make_shared<OutSock>(this, str);
           });
      return;
    }

    auto factory = File::iface<iface::Factory<Value>>(f);
    if (factory) {
      in_.clear();
      const auto itr = std::find_if(
          out_.begin(), out_.end(), [](auto& e) { return e->name() == "out"; });
      if (itr != out_.end()) {
        std::swap(out_[0], *itr);
        out_.resize(1);
      } else {
        out_ = { std::make_shared<OutSock>(this, "out") };
      }
      return;
    }
  }
  void SyncSocks(const std::string& base) noexcept {
    try {
      SyncSocks(&*RefStack().Resolve(base).Resolve(path_));
    } catch (NotFoundException&) {
      in_.clear();
      out_.clear();
    }
  }


  // A watcher for a context of lambda execution
  class LambdaContext final : public Context {
   public:
    LambdaContext(Ref* o, Node* node, const std::shared_ptr<Context>& outctx) :
        owner_(o), life_(o->life_), node_(node), outctx_(outctx) {
    }
    void ObserveSend(const OutSock& sock, const Value& v) noexcept override {
      if (life_.expired()) return;

      auto outctx = outctx_.lock();
      if (!outctx) return;

      if (&sock.owner() == node_) {
        auto dst = owner_->FindOut(sock.name());
        if (dst) dst->Send(outctx, Value(v));
      }
    }

   private:
    Ref* owner_;

    std::weak_ptr<std::monostate> life_;

    Node* node_;

    std::weak_ptr<Context> outctx_;
  };


  // An input pin which redirects to the entity
  class Input : public InSock {
   public:
    Input(Ref* o, std::string_view n) :
        InSock(o, n), owner_(o), life_(o->life_) {
    }
    void Receive(const std::shared_ptr<Context>& ctx, Value&& v) noexcept override {
      if (life_.expired()) return;

      try {
        auto node = File::iface<Node>(
            *RefStack().Resolve(owner_->basepath_).Resolve(owner_->path_));
        if (!node) throw Exception("missing entity");

        auto dst = node->FindIn(name());
        if (!dst) throw Exception("socket mismatch");

        auto data = ctx->GetOrNew<Data>(owner_, owner_, node, ctx);
        dst->Receive(data->ctx(), std::move(v));

      } catch (Exception& e) {
        notify::Error(owner_->basepath_, owner_, e.msg());
      }
    }

   private:
    Ref* owner_;

    std::weak_ptr<std::monostate> life_;


    // one Data per one lambda execution
    class Data final : public Context::Data {
     public:
      Data() = delete;
      Data(Ref* o, Node* node, const std::shared_ptr<Context>& ctx) noexcept :
          ctx_(std::make_unique<LambdaContext>(o, node, ctx)) {
      }

      const std::shared_ptr<LambdaContext>& ctx() noexcept { return ctx_; }

     private:
      std::shared_ptr<LambdaContext> ctx_;
    };
  };
};
void Ref::Update(RefStack& ref, const std::shared_ptr<Context>&) noexcept {
  ImGui::TextUnformatted("REF");

  const auto line = ImGui::GetCursorPosY();
  ImGui::SetCursorPosY(line + ImGui::GetFrameHeightWithSpacing());

  auto f = &*ref.Resolve(path_);
  ImGui::BeginGroup();
  if (f == this || ref.size() > 256) {
    ImGui::TextDisabled("recursive reference");
    in_.clear();
    out_.clear();
  } else {
    ref.Push({"@", f});
    ImGui::PushID(f);
    UpdateEntity(ref, f);
    ImGui::PopID();
    ref.Pop();
  }
  ImGui::EndGroup();

  ImGui::SetCursorPosY(line);
  ImGui::Button(("-> "s+(path_.size()? path_: "(empty)"s)).c_str(),
                {ImGui::GetItemRectSize().x, 0});
  if (path_.size() && ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", path_.c_str());
  }

  gui::NodeCanvasResetZoom();
  if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
    UpdateMenu(ref, ctx_);
    ImGui::EndPopup();
  }
  gui::NodeCanvasSetZoom();
}
void Ref::UpdateMenu(RefStack& ref, const std::shared_ptr<Context>&) noexcept {
  if (ImGui::MenuItem("Re-sync")) {
    Queue::main().Push([this, p = ref.Stringify()]() { SyncSocks(p); });
  }
  ImGui::Separator();
  if (ImGui::BeginMenu("path")) {
    if (gui::InputPathMenu(ref, &path_editing_, &path_)) {
      Queue::main().Push([this, p = ref.Stringify()]() { SyncSocks(p); });
    }
    ImGui::EndMenu();
  }
}
void Ref::UpdateEntity(RefStack& ref, File* f) noexcept {
  if (f == this) return;
  const auto em = ImGui::GetFontSize();

  auto node = File::iface<Node>(f);
  if (node) {
    node->Update(ref, ctx_);
    return;
  }

  auto factory = File::iface<iface::Factory<Value>>(f);
  if (factory) {
    static const char* kTitle = "Value Factory";

    const auto w = ImGui::CalcTextSize(kTitle).x;
    ImGui::TextUnformatted(kTitle);

    const auto wt = ImGui::CalcTextSize("out").x + em;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+w-wt);
    ImGui::TextUnformatted("out");
    ImGui::SameLine();
    if (ImNodes::BeginOutputSlot("out", 1)) {
      gui::NodeSocket();
      ImNodes::EndSlot();
    }
    return;
  }
}

} }  // namespace kingtaker
