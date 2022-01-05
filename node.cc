#define IMGUI_DEFINE_MATH_OPERATORS

#include "kingtaker.hh"

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <ImNodes.h>
#include <linalg.hh>

#include "iface/dir.hh"
#include "iface/gui.hh"
#include "iface/history.hh"
#include "iface/node.hh"
#include "iface/queue.hh"

namespace kingtaker {

class NodeNet : public File {
 public:
  static inline TypeInfo type_ = TypeInfo::New<NodeNet>(
      "NodeNet", "node network",
      {"GenericDir"},
      {typeid(iface::DirItem), typeid(iface::GUI), typeid(iface::History)});

  using IndexMap = std::unordered_map<iface::Node*, size_t>;
  using NodeMap  = std::vector<iface::Node*>;

  struct Conn final {
    std::weak_ptr<iface::Node::InSock>  in;
    std::weak_ptr<iface::Node::OutSock> out;
  };
  using ConnList = std::vector<Conn>;

  struct NodeHolder;
  using NodeHolderList    = std::vector<std::unique_ptr<NodeHolder>>;
  using NodeHolderRefList = std::vector<NodeHolder*>;

  struct NodeHolder final {
   public:
    static std::unique_ptr<NodeHolder> Create(size_t id, std::unique_ptr<File>&& f) {
      auto n = f->iface<iface::Node>();
      if (!n) return nullptr;
      return std::make_unique<NodeHolder>(id, std::move(f), n);
    }

    NodeHolder() = delete;
    NodeHolder(size_t id, std::unique_ptr<File>&& f, iface::Node* n) noexcept :
        id_(id), file_(std::move(f)), entity_(n), first_(true) {
      assert(file_);
      assert(entity_);
    }
    NodeHolder(size_t                  id,
               std::unique_ptr<File>&& f,
               iface::Node*            n,
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
        auto n = f->iface<iface::Node>();
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
          if (!out) throw DeserializeException("missing out socket: "+out_name);

          for (size_t j = 0; j < out_dst.via.array.size; ++j) {
            std::pair<size_t, std::string> conn;
            out_dst.via.array.ptr[j].convert(conn);
            if (conn.first >= nmap.size()) throw DeserializeException("missing node");

            auto in = nmap[conn.first]->FindIn(conn.second);
            if (!in) throw DeserializeException("missing in socket: "+conn.second);

            iface::Node::Link(out, in);
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

    void UpdateWindow(RefStack& ref) noexcept {
      ref.Push({std::to_string(id_), file_.get()});
      ImGui::PushID(file_.get());

      File::iface<iface::GUI>(*file_, iface::GUI::null()).Update(ref);

      ImGui::PopID();
      ref.Pop();
    }
    void UpdateNode(NodeNet* owner, RefStack& ref) noexcept {
      ref.Push({std::to_string(id_), file_.get()});
      ImGui::PushID(file_.get());

      if (first_) {
        ImNodes::AutoPositionNode(this);
        first_ = false;
      }

      if (ImNodes::BeginNode(this, &pos_, &select_)) {
        entity_->Update(ref);
      }
      ImNodes::EndNode();

      if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Clone")) {
          owner->history_.AddNodeIf(Clone(owner->next_id_++));
        }
        if (ImGui::MenuItem("Remove")) {
          owner->history_.RemoveNodes({this});
        }
        if (entity_->flags() & iface::Node::kMenu) {
          ImGui::Separator();
          entity_->UpdateMenu(ref);
        }
        ImGui::EndPopup();
      }
      ImGui::PopID();
      ref.Pop();
    }

    size_t id() const noexcept { return id_; }
    File& file() const noexcept { return *file_; }
    iface::Node& entity() const noexcept { return *entity_; }

   private:
    size_t id_;

    std::unique_ptr<File> file_;

    iface::Node* entity_;

    ImVec2 pos_ = {0, 0};

    bool select_ = false;

    bool first_ = false;
  };

  NodeNet() noexcept : NodeNet(Clock::now()) { }
  NodeNet(Time lastmod, NodeHolderList&& nodes = {}, size_t next = 0) noexcept :
      File(&type_),
      lastmod_(lastmod), nodes_(std::move(nodes)),
      next_id_(next), gui_(this), history_(this) {
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
      return ret;

    } catch (msgpack::type_error& e) {
      throw DeserializeException("broken data structure");
    }
  }
  void Serialize(Packer& pk) const noexcept override {
    std::unordered_map<iface::Node*, size_t> idxmap;

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
    std::unordered_map<iface::Node*, iface::Node*> nmap;

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

          iface::Node::Link(dstoutsock, dstinsock);
        }
      }
    }
    return std::make_unique<NodeNet>(Clock::now(), std::move(nodes));
  }

  Time lastModified() const noexcept override {
    return lastmod_;
  }
  void* iface(const std::type_index& t) noexcept override {
    if (typeid(iface::DirItem) == t) return static_cast<iface::DirItem*>(&gui_);
    if (typeid(iface::GUI)     == t) return static_cast<iface::GUI*>(&gui_);
    if (typeid(iface::History) == t) return &history_;
    return nullptr;
  }

 private:
  NodeHolder& FindHolder(const iface::Node& n) noexcept {
    for (auto& h: nodes_) {
      if (&h->entity() == &n) return *h;
    }
    assert(false);
  }

  Time lastmod_;

  NodeHolderList nodes_;

  size_t next_id_ = 0;

  class GUI final : public iface::GUI, public iface::DirItem {
   public:
    GUI(NodeNet* owner) : DirItem(kMenu), owner_(owner) {
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
      if (ImGui::BeginPopupContextItem()) {
        if (ImGui::BeginMenu("New")) {
          for (auto& p : File::registry()) {
            auto& t = *p.second;
            if (!t.factory() || !t.CheckTagged("NodeNet")) continue;
            if (ImGui::MenuItem(t.name().c_str())) {
              owner_->history_.AddNodeIf(
                  NodeHolder::Create(owner_->next_id_++, t.Create()));
            }
            if (ImGui::IsItemHovered()) {
              ImGui::SetTooltip(t.desc().c_str());
            }
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
        ImGui::SetWindowFontScale(canvas_.Zoom);
        ImGui::EndPopup();
      }

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
      if (rm_conns.size()) owner_->history_.Unlink(std::move(rm_conns));

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
      ImNodes::EndCanvas();
    }

   private:
    NodeNet* owner_;

    bool shown_ = false;

    ImNodes::CanvasState canvas_;
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
          if (!iface::Node::Link(conn.out, conn.in)) {
            throw Exception("cannot link deleted socket");
          }
        }
      }
      void Unlink() const {
        for (auto& conn : conns_) {
          if (!iface::Node::Unlink(conn.out, conn.in)) {
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
};

}  // namespace kingtaker
