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

#include "iface/gui.hh"
#include "iface/history.hh"
#include "iface/node.hh"
#include "iface/queue.hh"

namespace kingtaker {

class NodeNet : public File {
 public:
  static inline TypeInfo* type_ = TypeInfo::New<NodeNet>(
      "NodeNet", "node network");

  using IndexMap = std::unordered_map<iface::Node*, size_t>;
  using NodeMap  = std::vector<iface::Node*>;
  using LinkSet  = std::vector<std::pair<iface::Node::InSock*, iface::Node::OutSock*>>;

  struct NodeHolder final {
   public:
    static std::unique_ptr<NodeHolder> Create(std::unique_ptr<File>&& f) {
      auto n = f->iface<iface::Node>();
      if (!n) return nullptr;
      return std::make_unique<NodeHolder>(std::move(f), n);
    }

    NodeHolder() = delete;
    NodeHolder(std::unique_ptr<File>&& f, iface::Node* n) noexcept :
        file_(std::move(f)), entity_(n), first_(true) {
      assert(file_);
      assert(entity_);
    }
    NodeHolder(std::unique_ptr<File>&& f,
               iface::Node*            n,
               ImVec2&&                p,
               bool                    sel) noexcept :
        file_(std::move(f)), entity_(n), pos_(std::move(p)), select_(sel) {
      assert(file_);
      assert(entity_);
    }
    NodeHolder(const NodeHolder&) = delete;
    NodeHolder(NodeHolder&&) = delete;
    NodeHolder& operator=(const NodeHolder&) = delete;
    NodeHolder& operator=(NodeHolder&&) = delete;

    static std::unique_ptr<NodeHolder> Deserialize(const msgpack::object& obj) {
      try {
        auto f = File::Deserialize(msgpack::find(obj, "file"s));
        auto n = f->iface<iface::Node>();
        if (!n) throw DeserializeException("it's not node");

        std::pair<float, float> p;
        msgpack::find(obj, "pos"s).convert(p);

        const bool sel = msgpack::find(obj, "select"s).as<bool>();

        return std::make_unique<NodeHolder>(
            std::move(f), n, ImVec2 {p.first, p.second}, sel);
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

            out->Link(*in);
          }
        }
      } catch (msgpack::type_error& e) {
        throw DeserializeException("broken NodeHolder link");
      }
    }
    void Serialize(Packer& pk) const noexcept {
      pk.pack_map(3);

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
        for (auto in : out->dst()) {
          auto itr = idxmap.find(&in->owner());
          if (itr == idxmap.end()) continue;
          pk.pack(std::make_tuple(itr->second, in->name()));
        }
      }
    }
    std::unique_ptr<NodeHolder> Clone() const noexcept {
      return NodeHolder::Create(file_->Clone());
    }

    LinkSet SaveLinks() const noexcept {
      LinkSet ret;
      for (auto in : entity_->in()) {
        for (auto out : in->src()) ret.emplace_back(in, out);
      }
      for (auto out : entity_->out()) {
        for (auto in : out->dst()) ret.emplace_back(in, out);
      }
      return ret;
    }
    void Isolate() noexcept {
      for (auto in : entity_->in()) {
        for (auto out : in->src()) out->Unlink(*in);
      }
      for (auto out : entity_->out()) {
        for (auto in : out->dst()) out->Unlink(*in);
      }
    }

    void UpdateNode(NodeNet* owner, RefStack& ref) noexcept {
      auto gui = file_->iface<iface::GUI>();

      if (first_) {
        ImNodes::AutoPositionNode(this);
        first_ = false;
      }

      if (ImNodes::BeginNode(this, &pos_, &select_)) {
        if (gui && (gui->feats() & iface::GUI::kNode)) {
          gui->UpdateNode(ref);
        } else {
          // TODO(falsycat)
        }
      }
      ImNodes::EndNode();

      if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Clone")) {
          auto h = Clone();
          if (h) {
            owner->history_.Queue(
                std::make_unique<SwapCommand>(owner, std::move(h)));
          }
        }
        if (ImGui::MenuItem("Remove")) {
          owner->history_.Queue(std::make_unique<SwapCommand>(owner, this));
        }
        ImGui::EndPopup();
      }
    }

    File& file() const noexcept { return *file_; }
    iface::Node& entity() const noexcept { return *entity_; }

    ImVec2& pos() noexcept { return pos_; }

   private:
    std::unique_ptr<File> file_;

    iface::Node* entity_;

    ImVec2 pos_ = {0, 0};

    bool select_ = false;

    bool first_ = false;
  };

  NodeNet() noexcept : NodeNet(Clock::now()) { }
  NodeNet(Time lastmod, std::vector<std::unique_ptr<NodeHolder>>&& nodes = {}) noexcept :
      File(type_),
      lastmod_(lastmod), nodes_(std::move(nodes)), gui_(this) {
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj) {
    try {
      const auto lastmod = Clock::from_time_t(msgpack::find(obj, "lastMod"s).as<time_t>());

      auto& obj_nodes = msgpack::find(obj, "nodes"s);
      if (obj_nodes.type != msgpack::type::ARRAY) throw msgpack::type_error();

      NodeMap nmap(obj_nodes.via.array.size);
      std::vector<std::unique_ptr<NodeHolder>> nodes(obj_nodes.via.array.size);

      for (size_t i = 0; i < obj_nodes.via.array.size; ++i) {
        nodes[i] = NodeHolder::Deserialize(obj_nodes.via.array.ptr[i]);
        nmap[i]  = &nodes[i]->entity();
      }

      auto& obj_links = msgpack::find(obj, "links"s);
      if (obj_links.type != msgpack::type::ARRAY) throw msgpack::type_error();
      if (obj_links.via.array.size > nmap.size()) {
        throw DeserializeException("broken NodeNet");
      }
      for (size_t i = 0; i < obj_links.via.array.size; ++i) {
        nodes[i]->DeserializeLink(obj_links.via.array.ptr[i], nmap);
      }

      auto ret = std::make_unique<NodeNet>(lastmod, std::move(nodes));
      ret->gui_.Deserialize(msgpack::find(obj, "gui"s));
      return ret;

    } catch (msgpack::type_error& e) {
      throw DeserializeException("broken data structure");
    }
  }
  void Serialize(Packer& pk) const noexcept override {
    std::unordered_map<iface::Node*, size_t> idxmap;

    pk.pack_map(4);

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

    pk.pack("gui"s);
    gui_.Serialize(pk);
  }
  std::unique_ptr<File> Clone() const noexcept override {
    std::unordered_map<iface::Node*, iface::Node*> nmap;

    std::vector<std::unique_ptr<NodeHolder>> nodes;
    nodes.reserve(nodes_.size());
    for (auto& h : nodes_) {
      nodes.push_back(h->Clone());
      nmap[&h->entity()] = &nodes.back()->entity();
    }
    for (auto& h : nodes) {
      auto srcn = &h->entity();
      auto dstn = nmap[srcn];

      for (auto srcoutsock : srcn->out()) {
        auto dstoutsock = dstn->FindOut(srcoutsock->name());
        if (!dstoutsock) continue;

        for (auto& srcinsock : srcoutsock->dst()) {
          auto dstinsock = nmap[&srcinsock->owner()]->FindIn(srcinsock->name());
          if (!dstinsock) continue;

          dstoutsock->Link(*dstinsock);
        }
      }
    }
    return std::make_unique<NodeNet>(Clock::now(), std::move(nodes));
  }

  Time lastModified() const noexcept override {
    return lastmod_;
  }
  void* iface(const std::type_index& t) noexcept override {
    return typeid(iface::GUI) == t? &gui_: nullptr;
  }

 private:
  NodeHolder& FindHolder(const iface::Node& n) noexcept {
    for (auto& h: nodes_) {
      if (&h->entity() == &n) return *h;
    }
    assert(false);
  }

  Time lastmod_;

  std::vector<std::unique_ptr<NodeHolder>> nodes_;

  class GUI final : public iface::GUI {
   public:
    GUI(NodeNet* owner) : owner_(owner) {
      canvas_.Style.NodeRounding = 0.f;
    }

    void Serialize(Packer& pk) const noexcept {
      pk.pack_map(2);

      pk.pack("zoom"s);
      pk.pack(canvas_.Zoom);

      pk.pack("offset"s);
      pk.pack(std::make_tuple(canvas_.Offset.x, canvas_.Offset.y));
    }
    void Deserialize(const msgpack::object& obj) noexcept {
      try {
        canvas_.Zoom = msgpack::find(obj, "zoom"s).as<float>();

        std::pair<float, float> offset;
        msgpack::find(obj, "offset"s).convert(offset);
        canvas_.Offset = {offset.first, offset.second};

      } catch (msgpack::type_error& e) {
        canvas_.Zoom   = 1.f;
        canvas_.Offset = {0, 0};
      }
    }

    void UpdateWindow(RefStack& ref) noexcept override {
      constexpr auto kFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
      const auto id = ref.Stringify() + ": NodeNet Editor";
      if (ImGui::Begin(id.c_str(), nullptr, kFlags)) {
        UpdateEditor(ref);
      }
      ImGui::End();
    }
    void UpdateEditor(RefStack& ref) noexcept override {
      ImNodes::BeginCanvas(&canvas_);
      if (ImGui::BeginPopupContextItem()) {
        ImGui::SetWindowFontScale(1);
        if (ImGui::BeginMenu("New")) {
          for (auto& p : File::registry()) {
            auto& t = *p.second;
            if (!t.hasFactory()) continue;
            if (ImGui::MenuItem(t.name())) {
              auto h = NodeHolder::Create(t.Create());
              if (h) {
                owner_->history_.Queue(
                    std::make_unique<SwapCommand>(owner_, std::move(h)));
              }
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

      for (size_t i = 0; i < owner_->nodes_.size(); ++i) {
        auto& h = owner_->nodes_[i];
        ref.Push(RefStack::Term {std::to_string(i), &h->file()});
        h->UpdateNode(owner_, ref);
        ref.Pop();
      }
      for (auto& h : owner_->nodes_) {
        auto node = &h->entity();

        for (auto src : node->out()) {
          std::vector<iface::Node::InSock*> rm;
          for (auto dst : src->dst()) {
            auto& srch = owner_->FindHolder(src->owner());
            auto  srcs = src->name().c_str();
            auto& dsth = owner_->FindHolder(dst->owner());
            auto  dsts = dst->name().c_str();
            if (!ImNodes::Connection(&dsth, dsts, &srch, srcs)) {
              rm.push_back(dst);
            }
          }
          for (auto dst : rm) src->Unlink(*dst);
        }
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
        if (src && dst) src->Link(*dst);
      }
      ImNodes::EndCanvas();
    }

   private:
    NodeNet* owner_;

    ImNodes::CanvasState canvas_;
  } gui_;

  class SwapCommand : public iface::History::Command {
   public:
    SwapCommand(NodeNet* o, std::unique_ptr<NodeHolder>&& h) :
        owner_(o), holder_(std::move(h)), ref_(holder_.get()) {
    }
    SwapCommand(NodeNet* o, NodeHolder* ref) :
        owner_(o), ref_(ref) {
    }

    void Exec() {
      auto& nodes = owner_->nodes_;
      if (holder_) {
        nodes.push_back(std::move(holder_));

      } else {
        auto itr = std::find_if(nodes.begin(), nodes.end(),
                                [this](auto& e) { return e.get() == ref_; });
        if (itr == nodes.end()) {
          throw Exception("target node is missing");
        }
        holder_ = std::move(*itr);
        nodes.erase(itr);
      }
    }
    void Apply() override { Exec(); }
    void Revert() override { Exec(); }

   private:
    NodeNet* owner_;

    std::unique_ptr<NodeHolder> holder_;

    NodeHolder* ref_ = nullptr;
  };
  iface::SimpleHistory<> history_;
};

}  // namespace kingtaker
