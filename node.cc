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
#include "iface/gui.hh"
#include "iface/history.hh"
#include "iface/node.hh"
#include "iface/queue.hh"

#include "util/gui.hh"

namespace kingtaker {
namespace {

class NodeNet : public File, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<NodeNet>(
      "NodeNet", "node network",
      {typeid(iface::DirItem), typeid(iface::GUI), typeid(iface::History)});

  using IndexMap = std::unordered_map<Node*, size_t>;
  using NodeMap  = std::vector<Node*>;

  struct Conn final {
    std::weak_ptr<Node::InSock>  in;
    std::weak_ptr<Node::OutSock> out;
  };
  using ConnList = std::vector<Conn>;

  struct NodeHolder;
  using NodeHolderList    = std::vector<std::unique_ptr<NodeHolder>>;
  using NodeHolderRefList = std::vector<NodeHolder*>;

  struct NodeHolder final {
   public:
    enum Type { kGeneric, kInput, kOutput };

    static std::unique_ptr<NodeHolder> Create(size_t id, std::unique_ptr<File>&& f) {
      auto n = f->iface<Node>();
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
    NodeHolder(const NodeHolder&) = delete;
    NodeHolder(NodeHolder&&) = delete;
    NodeHolder& operator=(const NodeHolder&) = delete;
    NodeHolder& operator=(NodeHolder&&) = delete;

    static std::unique_ptr<NodeHolder> Deserialize(const msgpack::object& obj) {
      try {
        const size_t id = msgpack::find(obj, "id"s).as<size_t>();

        auto f = File::Deserialize(msgpack::find(obj, "file"s));
        auto n = f->iface<Node>();
        if (!n) throw DeserializeException("it's not node");

        std::pair<float, float> p;
        msgpack::find(obj, "pos"s).convert(p);

        auto h = std::make_unique<NodeHolder>(
            id, std::move(f), n, ImVec2 {p.first, p.second});
        try {
          h->select_ = msgpack::find(obj, "select"s).as<bool>();
        } catch (msgpack::type_error& e) {
        }
        return h;

      } catch (msgpack::type_error& e) {
        throw DeserializeException("broken NodeHolder structure");
      }
    }
    void DeserializeLink(const msgpack::object& obj, const NodeMap& nmap) {
      try {
        if (obj.type != msgpack::type::MAP) throw msgpack::type_error();
        for (size_t i = 0; i < obj.via.map.size; ++i) {
          auto  out_name = obj.via.map.ptr[i].key.as<std::string>();
          auto& out_dst  = obj.via.map.ptr[i].val;

          auto out = entity_->FindOut(out_name);
          if (!out) continue;

          for (size_t j = 0; j < out_dst.via.array.size; ++j) {
            std::pair<size_t, std::string> conn;
            out_dst.via.array.ptr[j].convert(conn);
            if (conn.first >= nmap.size()) throw DeserializeException("missing node");

            auto in = nmap[conn.first]->FindIn(conn.second);
            if (!in) continue;

            Node::Link(out, in);
          }
        }
      } catch (msgpack::type_error& e) {
        throw DeserializeException("broken NodeHolder link");
      }
    }
    void Serialize(Packer& pk) const noexcept {
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
    void SerializeLink(Packer& pk, const IndexMap& idxmap) const noexcept {
      pk.pack_map(static_cast<uint32_t>(entity_->out().size()));
      for (auto out : entity_->out()) {
        pk.pack(out->name());
        pk.pack_array(static_cast<uint32_t>(out->dst().size()));

        out->CleanConns();
        for (auto& inw : out->dst()) {
          auto in  = inw.lock();
          auto itr = idxmap.find(&in->owner());
          if (itr == idxmap.end()) continue;
          pk.pack(std::make_tuple(itr->second, in->name()));
        }
      }
    }
    std::unique_ptr<NodeHolder> Clone(size_t id) const noexcept {
      return NodeHolder::Create(id, file_->Clone());
    }

    void Setup(NodeNet* owner) noexcept {
      auto inter = dynamic_cast<InternalNode*>(file_.get());
      if (inter) inter->Setup(owner);
    }
    void Teardown(NodeNet* owner) noexcept {
      auto inter = dynamic_cast<InternalNode*>(file_.get());
      if (inter) inter->Teardown(owner);
    }

    void UpdateNode(NodeNet* owner, RefStack& ref) noexcept {
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
          owner->history_.AddNodeIf(Clone(owner->next_id_++));
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
    void UpdateWindow(RefStack& ref) noexcept {
      ref.Push({std::to_string(id_), file_.get()});
      ImGui::PushID(file_.get());

      File::iface<iface::GUI>(*file_, iface::GUI::null()).Update(ref);

      ImGui::PopID();
      ref.Pop();
    }

    size_t id() const noexcept { return id_; }
    File& file() const noexcept { return *file_; }
    Node& entity() const noexcept { return *entity_; }

   private:
    size_t id_;

    std::unique_ptr<File> file_;

    Node* entity_;

    ImVec2 pos_ = {0, 0};

    bool select_ = false;

    bool first_ = false;
  };

  NodeNet() noexcept : NodeNet(Clock::now()) { }
  NodeNet(Time lastmod,
          NodeHolderList&& nodes = {},
          size_t next = 0) noexcept :
      File(&type_), Node(kNone),
      lastmod_(lastmod), nodes_(std::move(nodes)), next_id_(next),
      ctx_(std::make_shared<Context>()),
      gui_(this), history_(this) {
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj) {
    try {
      const auto lastmod = Clock::from_time_t(msgpack::find(obj, "lastMod"s).as<time_t>());

      const auto next = msgpack::find(obj, "nextId"s).as<size_t>();

      auto& obj_nodes = msgpack::find(obj, "nodes"s);
      if (obj_nodes.type != msgpack::type::ARRAY) throw msgpack::type_error();

      NodeMap nmap(obj_nodes.via.array.size);
      NodeHolderList nodes(obj_nodes.via.array.size);

      for (size_t i = 0; i < obj_nodes.via.array.size; ++i) {
        nodes[i] = NodeHolder::Deserialize(obj_nodes.via.array.ptr[i]);
        nmap[i]  = &nodes[i]->entity();

        if (nodes[i]->id() == next) {
          throw DeserializeException("nodeId conflict");
        }
      }

      auto& obj_links = msgpack::find(obj, "links"s);
      if (obj_links.type != msgpack::type::ARRAY) throw msgpack::type_error();
      if (obj_links.via.array.size > nmap.size()) {
        throw DeserializeException("broken NodeNet");
      }
      for (size_t i = 0; i < obj_links.via.array.size; ++i) {
        nodes[i]->DeserializeLink(obj_links.via.array.ptr[i], nmap);
      }

      auto ret = std::make_unique<NodeNet>(lastmod, std::move(nodes), next);
      ret->gui_.Deserialize(msgpack::find(obj, "gui"s));
      for (auto& h : ret->nodes_) h->Setup(ret.get());
      return ret;

    } catch (msgpack::type_error& e) {
      throw DeserializeException("broken data structure");
    }
  }
  void Serialize(Packer& pk) const noexcept override {
    std::unordered_map<Node*, size_t> idxmap;

    pk.pack_map(5);

    pk.pack("lastMod"s);
    pk.pack(Clock::to_time_t(lastmod_));

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
    gui_.Serialize(pk);
  }
  std::unique_ptr<File> Clone() const noexcept override {
    std::unordered_map<Node*, Node*> nmap;

    size_t id = 0;

    NodeHolderList nodes;
    nodes.reserve(nodes_.size());
    for (auto& h : nodes_) {
      nodes.push_back(h->Clone(id++));
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
    return std::make_unique<NodeNet>(Clock::now(), std::move(nodes));
  }

  void Update(RefStack&, const std::shared_ptr<Context>&) noexcept override {
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

    static const char* title = "NodeNet";
    const auto w  = ImGui::GetItemRectSize().x;
    const auto tw = ImGui::CalcTextSize(title).x;
    if (w > tw) {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w-tw)/2);
    }
    ImGui::SetCursorPosY(line);
    ImGui::TextUnformatted("NodeNet");
  }

  Time lastModified() const noexcept override {
    return lastmod_;
  }
  void* iface(const std::type_index& t) noexcept override {
    if (typeid(iface::DirItem) == t) return static_cast<iface::DirItem*>(&gui_);
    if (typeid(iface::GUI)     == t) return static_cast<iface::GUI*>(&gui_);
    if (typeid(iface::History) == t) return &history_;
    if (typeid(iface::Node)    == t) return static_cast<iface::Node*>(this);
    return nullptr;
  }

 private:
  NodeHolder& FindHolder(const Node& n) noexcept {
    for (auto& h: nodes_) {
      if (&h->entity() == &n) return *h;
    }
    assert(false);
  }

  Time lastmod_;

  NodeHolderList nodes_;

  size_t next_id_ = 0;

  std::shared_ptr<Context> ctx_;

  class GUI final : public iface::GUI, public iface::DirItem {
   public:
    GUI(NodeNet* o) : DirItem(kMenu), owner_(o) {
      canvas_.Style.NodeRounding = 0.f;
    }

    void Serialize(Packer& pk) const noexcept {
      pk.pack_map(3);

      pk.pack("shown"s);
      pk.pack(shown_);

      pk.pack("zoom"s);
      pk.pack(canvas_.Zoom);

      pk.pack("offset"s);
      pk.pack(std::make_tuple(canvas_.Offset.x, canvas_.Offset.y));
    }
    void Deserialize(const msgpack::object& obj) noexcept {
      try {
        shown_ = msgpack::find(obj, "shown"s).as<bool>();

        canvas_.Zoom = msgpack::find(obj, "zoom"s).as<float>();

        std::pair<float, float> offset;
        msgpack::find(obj, "offset"s).convert(offset);
        canvas_.Offset = {offset.first, offset.second};

      } catch (msgpack::type_error& e) {
        shown_         = false;
        canvas_.Zoom   = 1.f;
        canvas_.Offset = {0, 0};
      }
    }

    void Update(RefStack& ref) noexcept override {
      for (auto& h : owner_->nodes_) {
        h->UpdateWindow(ref);
      }

      if (shown_) {
        constexpr auto kFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

        ImGui::SetNextWindowSize(ImVec2 {24.f, 24.f}*ImGui::GetFontSize(),
                                 ImGuiCond_FirstUseEver);

        const auto id = ref.Stringify() + ": NodeNet Editor";
        if (ImGui::Begin(id.c_str(), nullptr, kFlags)) {
          UpdateCanvas(ref);
        }
        ImGui::End();
      }
    }
    void UpdateMenu(RefStack&) noexcept override {
      ImGui::MenuItem("NodeNet Editor", nullptr, &shown_);
    }

    void UpdateCanvas(RefStack& ref) noexcept {
      ImNodes::BeginCanvas(&canvas_);
      gui::NodeCanvasSetZoom();

      gui::NodeCanvasResetZoom();
      if (ImGui::BeginPopupContextItem()) {
        if (ImGui::BeginMenu("New")) {
          for (auto& p : File::registry()) {
            auto& t = *p.second;
            if (!t.factory() || !t.CheckImplemented<Node>()) continue;
            if (ImGui::MenuItem(t.name().c_str())) {
              owner_->history_.AddNodeIf(
                  NodeHolder::Create(owner_->next_id_++, t.Create()));
            }
            if (ImGui::IsItemHovered()) {
              ImGui::SetTooltip(t.desc().c_str());
            }
          }
          ImGui::Separator();
          if (ImGui::BeginMenu("Input")) {
            UpdateNewIO<InputNode>(owner_->in_);
            ImGui::EndMenu();
          }
          if (ImGui::BeginMenu("Output")) {
            UpdateNewIO<OutputNode>(owner_->out_);
            ImGui::EndMenu();
          }
          ImGui::EndMenu();
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Undo")) {
          owner_->history_.Move(-1);
        }
        if (ImGui::MenuItem("Redo")) {
          owner_->history_.Move(1);
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Clear history")) {
          owner_->history_.Clear();
        }
        if (ImGui::MenuItem("Clear entire context")) {
          File::QueueMainTask([this]() { owner_->ctx_ = std::make_shared<Context>(); });
        }
        ImGui::EndPopup();
      }
      gui::NodeCanvasSetZoom();

      for (auto& h : owner_->nodes_) {
        h->UpdateNode(owner_, ref);
      }

      ConnList rm_conns;
      for (auto& h : owner_->nodes_) {
        auto node = &h->entity();
        for (auto src : node->out()) {
          for (auto w_dst : src->dst()) {
            auto dst = w_dst.lock();
            if (!dst) continue;

            auto& srch = owner_->FindHolder(src->owner());
            auto  srcs = src->name().c_str();
            auto& dsth = owner_->FindHolder(dst->owner());
            auto  dsts = dst->name().c_str();
            if (!ImNodes::Connection(&dsth, dsts, &srch, srcs)) {
              rm_conns.emplace_back(dst, src);
            }
          }
        }
      }
      if (rm_conns.size()) {
        owner_->history_.Unlink(std::move(rm_conns));
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
        if (src && dst) owner_->history_.Link({{dst, src}});
      }

      gui::NodeCanvasResetZoom();
      ImNodes::EndCanvas();
    }

    template <typename T, typename U>
    void UpdateNewIO(std::vector<std::shared_ptr<U>>& list) noexcept {
      constexpr auto kFlags =
          ImGuiInputTextFlags_EnterReturnsTrue |
          ImGuiInputTextFlags_AutoSelectAll;

      static const char* kHint = "enter to add...";

      ImGui::SetKeyboardFocusHere();
      const bool submit = ImGui::InputTextWithHint("##newIO", kHint, &io_name_, kFlags);

      const bool empty = io_name_.empty();
      const bool dup   = list.end() !=
          std::find_if(list.begin(), list.end(),
                       [this](auto& e) { return e->name() == io_name_; });

      if (empty) {
        ImGui::Bullet();
        ImGui::TextUnformatted("empty name");
      }
      if (dup) {
        ImGui::Bullet();
        ImGui::TextUnformatted("name duplication");
      }
      if (submit && !empty && !dup) {
        owner_->history_.AddNodeIf(
            NodeHolder::Create(owner_->next_id_++, std::make_unique<T>(io_name_)));
        io_name_ = "";
        ImGui::CloseCurrentPopup();
      }
    }

   private:
    NodeNet* owner_;

    ImNodes::CanvasState canvas_;

    bool shown_ = false;

    std::string io_name_;
  } gui_;

  class History : public iface::SimpleHistory<> {
   public:
    History(NodeNet* o) : owner_(o) { }

    void AddNodeIf(std::unique_ptr<NodeHolder>&& h) noexcept {
      if (!h) return;

      NodeHolderList list;
      list.push_back(std::move(h));
      AddNodes(std::move(list));
    }
    void AddNodes(NodeHolderList&& h) noexcept {
      prev_ = {};
      Queue(std::make_unique<SwapCommand>(owner_, std::move(h)));
    }
    void RemoveNodes(NodeHolderRefList&& h) noexcept {
      prev_ = {};
      Queue(std::make_unique<SwapCommand>(owner_, std::move(h)));
    }
    void Link(ConnList&& conns) noexcept {
      prev_ = {};
      Queue(std::make_unique<LinkSwapCommand>(
              owner_, LinkSwapCommand::kLink, std::move(conns)));
    }
    void Unlink(ConnList&& conns) noexcept {
      prev_ = {};
      Queue(std::make_unique<LinkSwapCommand>(
              owner_, LinkSwapCommand::kUnlink, std::move(conns)));
    }

   private:
    NodeNet* owner_;

    class LinkSwapCommand : public Command {
     public:
      enum Type {
        kLink,
        kUnlink,
      };

      LinkSwapCommand(NodeNet* o, Type t, ConnList&& conns) :
          owner_(o), type_(t), conns_(std::move(conns)) {
      }

      void Link() const {
        for (auto& conn : conns_) {
          if (!Node::Link(owner_->ctx_, conn.out, conn.in)) {
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
      NodeNet* owner_;

      Type type_;

      ConnList conns_;
    };
    class SwapCommand : public Command {
     public:
      SwapCommand(NodeNet* o, NodeHolderList&& h = {}) :
          owner_(o), holders_(std::move(h)) {
        refs_.reserve(holders_.size());
        for (auto& holder : holders_) refs_.push_back(holder.get());
      }
      SwapCommand(NodeNet* o, NodeHolderRefList&& refs = {}) :
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
                                    [this, r](auto& e) { return e.get() == r; });
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
        links_.emplace(owner_, LinkSwapCommand::kUnlink, std::move(conns));
      }

      NodeNet* owner_;

      NodeHolderList holders_;

      NodeHolderRefList refs_;

      std::optional<LinkSwapCommand> links_;
    };

    std::variant<std::monostate> prev_;
  } history_;

  class InternalNode {
   public:
    InternalNode() = default;
    virtual ~InternalNode() = default;

    virtual void Setup(NodeNet*) noexcept { }
    virtual void Teardown(NodeNet*) noexcept { }
  };

  template <typename T>
  class AbstractIONode : public File, public Node, public InternalNode {
   public:
    class CtxSock : public T {
     public:
      CtxSock(NodeNet* owner, std::string_view name) : T(owner, name) {
      }

      void Attach(Node* n) noexcept {
        nodes_.insert(n);
      }
      bool Detach(Node* n) noexcept {
        nodes_.erase(n);
        return nodes_.empty();
      }

     protected:
      std::unordered_set<Node*> nodes_;
    };
    struct Data final {
     public:
      Data() = default;

      std::shared_ptr<CtxSock> sock;
    };

    AbstractIONode(TypeInfo* t, std::string_view name) :
        File(t), Node(kNone), name_(name), data_(std::make_shared<Data>()) {
    }

    void Serialize(Packer& pk) const noexcept override {
      pk.pack(name_);
    }

    void Setup(NodeNet* owner) noexcept override {
      auto& list = GetList(owner);
      auto itr = std::find_if(
          list.begin(), list.end(), [this](auto& e) { return e->name() == name_; });
      if (itr != list.end()) {
        data_->sock = std::dynamic_pointer_cast<CtxSock>(*itr);
      } else {
        auto sock = Create(owner, name_);
        data_->sock = sock;

        list.push_back(sock);
        std::sort(list.begin(), list.end(), [](auto& a, auto& b) {
                    return a->name() < b->name();
                  });
      }

      assert(data_->sock);
      data_->sock->Attach(this);
    }
    void Teardown(NodeNet* owner) noexcept override {
      auto& list = GetList(owner);

      if (data_->sock->Detach(this)) {
        auto itr = std::find(list.begin(), list.end(), data_->sock);
        if (itr != list.end()) list.erase(itr);
      }
      data_->sock = nullptr;
    }

    Time lastModified() const noexcept override { return {}; }
    void* iface(const std::type_index& t) noexcept override {
      if (typeid(Node) == t) return static_cast<iface::Node*>(this);
      return nullptr;
    }

   protected:
    virtual std::vector<std::shared_ptr<T>>& GetList(NodeNet*) const noexcept = 0;
    virtual std::shared_ptr<CtxSock> Create(NodeNet*, std::string_view) const noexcept = 0;

    using Node::in_;
    using Node::out_;

    std::string name_;

    std::shared_ptr<Data> data_;
  };
  class InputNode : public AbstractIONode<InSock> {
   public:
    static inline TypeInfo type_ = TypeInfo::New<InputNode>(
        "NodeNet_InputNode", "input emitter in NodeNet", {});

    InputNode(std::string_view name) noexcept : AbstractIONode(&type_, name) {
      out_.emplace_back(new OutSock(this, "out"));
    }

    static std::unique_ptr<File> Deserialize(const msgpack::object& obj) {
      return std::make_unique<InputNode>(obj.as<std::string>());
    }
    std::unique_ptr<File> Clone() const noexcept {
      return std::make_unique<InputNode>(name_);
    }

    void Update(RefStack& ref, const std::shared_ptr<Context>&) noexcept override {
      auto owner = ref.FindParent<NodeNet>();
      if (!owner) {
        ImGui::TextUnformatted("INPUT");
        ImGui::TextUnformatted("ERROR X(");
        ImGui::TextUnformatted("This node must be used at inside of NodeNet");
        return;
      }

      ImGui::Text("IN> %s", data_->sock->name().c_str());

      ImGui::SameLine();
      if (ImNodes::BeginOutputSlot("out", 1)) {
        gui::NodeSocket();
        ImNodes::EndSlot();
      }
    }

   private:
    class CtxInSock : public CtxSock {
     public:
      CtxInSock(NodeNet* o, std::string_view n) : CtxSock(o, n) {
      }
      void Receive(const std::shared_ptr<Context>& ctx, Value&& v) noexcept override {
        for (auto n : nodes_) n->out()[0]->Send(ctx, Value(v));
      }
    };

    std::vector<std::shared_ptr<InSock>>& GetList(NodeNet* owner) const noexcept override {
      return owner->in_;
    }
    std::shared_ptr<CtxSock> Create(NodeNet* o, std::string_view n) const noexcept override {
      return std::make_shared<CtxInSock>(o, n);
    }
  };
  class OutputNode : public AbstractIONode<OutSock> {
   public:
    static inline TypeInfo type_ = TypeInfo::New<OutputNode>(
        "NodeNet_OutputNode", "output receiver in NodeNet", {});

    OutputNode(std::string_view name) noexcept : AbstractIONode(&type_, name) {
      in_.emplace_back(new Receiver(this));
    }

    static std::unique_ptr<File> Deserialize(const msgpack::object& obj) {
      return std::make_unique<OutputNode>(obj.as<std::string>());
    }
    std::unique_ptr<File> Clone() const noexcept {
      return std::make_unique<OutputNode>(name_);
    }

    void Update(RefStack& ref, const std::shared_ptr<Context>&) noexcept override {
      auto owner = ref.FindParent<NodeNet>();
      if (!owner) {
        ImGui::TextUnformatted("OUTPUT");
        ImGui::TextUnformatted("ERROR X(");
        ImGui::TextUnformatted("This node must be used at inside of NodeNet");
        return;
      }

      if (ImNodes::BeginInputSlot("in", 1)) {
        gui::NodeSocket();
        ImNodes::EndSlot();
      }

      ImGui::SameLine();
      ImGui::Text("%s >OUT", data_->sock->name().c_str());
    }

   protected:
    class CtxOutSock : public CtxSock {
     public:
      CtxOutSock(NodeNet* o, std::string_view n) : CtxSock(o, n) {
      }
      void Send(const std::shared_ptr<Context>& ctx, Value&& v) noexcept override {
        ctx->Receive(name(), Value(v));
        CtxSock::Send(ctx, std::move(v));
      }
    };
    std::vector<std::shared_ptr<OutSock>>& GetList(NodeNet* owner) const noexcept override {
      return owner->out_;
    }
    std::shared_ptr<CtxSock> Create(NodeNet* o, std::string_view n) const noexcept override {
      return std::make_shared<CtxOutSock>(o, n);
    }

   private:
    class Receiver : public InSock {
     public:
      Receiver(OutputNode* o) noexcept : InSock(o, "in"), data_(o->data_) { }

      void Receive(const std::shared_ptr<Context>& ctx, Value&& v) noexcept override {
        if (data_->sock) data_->sock->Send(ctx, Value(v));
      }

     private:
      std::shared_ptr<Data> data_;
    };
  };
};

class RefNode : public File, public iface::Node {
 public:
  static inline TypeInfo type_ = TypeInfo::New<RefNode>(
      "RefNode", "uses node as lambda",
      {typeid(iface::Node)});

  using Life = std::weak_ptr<std::monostate>;

  RefNode(std::string_view path = "",
          const std::vector<std::string>& in  = {},
          const std::vector<std::string>& out = {}) noexcept :
      File(&type_), Node(kMenu),
      life_(std::make_shared<std::monostate>()), path_(path),
      ctx_(std::make_shared<Context>(std::make_unique<EditContextWatcher>(this, life_))) {
    in_.reserve(in.size());
    out_.reserve(out.size());

    const auto p = ParsePath(path);
    for (auto& v : in) in_.push_back(std::make_shared<Input>(this, v));
    for (auto& v : out) out_.push_back(std::make_shared<OutSock>(this, v));
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj) {
    auto str = msgpack::find(obj, "path"s).as<std::string>();
    auto in  = msgpack::find(obj, "in"s).as<std::vector<std::string>>();
    auto out = msgpack::find(obj, "out"s).as<std::vector<std::string>>();
    return std::make_unique<RefNode>(str, std::move(in), std::move(out));
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
  std::unique_ptr<File> Clone() const noexcept override {
    return std::make_unique<RefNode>(path_);
  }

  void Update(RefStack& ref, const std::shared_ptr<Context>&) noexcept override {
    ImGui::TextUnformatted("REF");

    const auto line = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(line + ImGui::GetFrameHeightWithSpacing());

    ImGui::BeginGroup();
    try {
      auto r = ref.Resolve(path_);
      auto n = FetchNode(this, r);

      ref.Push({"@", &*r});
      ImGui::PushID(n);

      n->Update(ref, ctx_);

      ImGui::PopID();
      ref.Pop();
    } catch (Exception& e) {
      ImGui::TextDisabled(e.msg().c_str());
      in_.clear();
      out_.clear();
    }
    ImGui::EndGroup();

    ImGui::SetCursorPosY(line);
    ImGui::Button(("-> "s+(path_.size()? path_: "(empty)"s)).c_str(),
                  {ImGui::GetItemRectSize().x, 0});

    gui::NodeCanvasResetZoom();
    if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
      UpdateMenu(ref, ctx_);
      ImGui::EndPopup();
    }
    gui::NodeCanvasSetZoom();
  }
  void UpdateMenu(RefStack& ref, const std::shared_ptr<Context>& ctx) noexcept override {
    if (ImGui::BeginMenu("Change")) {
      constexpr auto kFlags = 
          ImGuiInputTextFlags_EnterReturnsTrue |
          ImGuiInputTextFlags_AutoSelectAll;
      static const char* const kHint = "enter new path...";

      ImGui::SetKeyboardFocusHere();
      const bool submit = ImGui::InputTextWithHint(
          "##renamer", kHint, &path_editing_, kFlags);
      try {
        auto newref = ref.Resolve(path_editing_);
        if (submit) {
          ImGui::CloseCurrentPopup();

          path_editing_ = newref.Stringify();
          File::QueueMainTask(
              [this, fullpath = newref.Stringify()]() {
                try {
                  SyncSocks(FetchNode(nullptr, RefStack().Resolve(fullpath)));
                  path_ = fullpath;
                } catch (Exception& e) {
                  ctx_->Inform(e.msg());
                }
              });
        }
      } catch (NotFoundException& e) {
        ImGui::Bullet();
        ImGui::TextUnformatted("file not found");
      } catch (Exception& e) {
        ImGui::Bullet();
        ImGui::TextUnformatted(e.msg().c_str());
      }

      ImGui::EndMenu();
    }

    try {
      auto nref = ref.Resolve(path_);
      auto n    = FetchNode(this, nref);

      if (ImGui::MenuItem("Re-sync")) {
        File::QueueMainTask(
            [this, fullpath = nref.Stringify()]() {
              try {
                SyncSocks(FetchNode(nullptr, RefStack().Resolve(fullpath)));
              } catch (Exception& e) {
                ctx_->Inform(e.msg());
              }
            });
      }
      if (n->flags() & kMenu) {
        if (ImGui::BeginMenu("Target")) {
          n->UpdateMenu(ref, ctx);
          ImGui::EndMenu();
        }
      }
    } catch (Exception& e) {
    }
  }

  Time lastModified() const noexcept override { return {}; }
  void* iface(const std::type_index& t) noexcept override {
    if (typeid(iface::Node) == t) return static_cast<iface::Node*>(this);
    return nullptr;
  }

 private:
  static Node* FetchNode(RefNode* owner, const RefStack& ref) {
    auto f = &*ref;
    if (f == owner || ref.size() > 256) {
      throw Exception("recursive reference");
    }

    auto n = File::iface<Node>(f);
    if (!n) throw Exception("target is not a node");
    return n;
  }
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
  void SyncSocks(Node* n) {
    try {
      Sync<InSock>(in_, n->in(), [this](auto str) {
             return std::make_shared<Input>(this, str);
           });
      Sync<OutSock>(out_, n->out(), [this](auto str) {
             return std::make_shared<OutSock>(this, str);
           });

    } catch (Exception& e) {
    }
  }

  std::shared_ptr<std::monostate> life_;

  std::string path_;

  std::shared_ptr<Context> ctx_;

  std::string path_editing_;

  class EditContextWatcher final : public ContextWatcher {
   public:
    EditContextWatcher(RefNode* o, const Life& life) noexcept : owner_(o), life_(life) {
    }

    void Inform(std::string_view msg) noexcept override {
      (void) msg;
      // TODO(falsycat)
    }

   private:
    RefNode* owner_;

    std::weak_ptr<std::monostate> life_;
  };

  class LambdaContextWatcher final : public ContextWatcher {
   public:
    LambdaContextWatcher(RefNode* o, const Life& life, const std::shared_ptr<Context>& outctx) :
        owner_(o), life_(life), outctx_(outctx) {
    }

    void Receive(std::string_view name, Value&& v) noexcept override {
      if (life_.expired()) return;

      auto outctx = outctx_.lock();
      if (!outctx) return;

      auto dst = owner_->FindOut(name);
      if (dst) dst->Send(outctx, std::move(v));
    }

   private:
    RefNode* owner_;

    std::weak_ptr<std::monostate> life_;

    std::weak_ptr<Context> outctx_;
  };

  class Data final : public Context::Data {
   public:
    Data() = delete;
    Data(RefNode* o, const Life& life, const std::shared_ptr<Context>& ctx) :
        ctx_(std::make_shared<Context>(
                std::make_unique<LambdaContextWatcher>(o, life, ctx))) {
    }

    const std::shared_ptr<Context>& ctx() noexcept { return ctx_; }

   private:
    std::shared_ptr<Context> ctx_;
  };

  class Input : public InSock {
   public:
    Input(RefNode* o, std::string_view n) :
        InSock(o, n), owner_(o), life_(o->life_) {
    }
    void Receive(const std::shared_ptr<Context>& ctx, Value&& v) noexcept override {
      if (life_.expired()) return;

      try {
        const auto ref = RefStack().Resolve(owner_->path_);

        auto dst = FetchNode(nullptr, ref)->FindIn(name());
        if (!dst) throw Exception("socket mismatch");

        auto& data = ctx->GetOrNew<Data>(owner_, owner_, life_, ctx);
        dst->Receive(data.ctx(), std::move(v));

      } catch (Exception& e) {
        ctx->Inform(e.msg());
      }
    }

   private:
    RefNode* owner_;

    std::weak_ptr<std::monostate> life_;
  };

};

} }  // namespace kingtaker
