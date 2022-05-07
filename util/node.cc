#include "util/node.hh"

#include <type_traits>
#include <unordered_set>

#include <msgpack.hh>


namespace kingtaker {

class NodeLinkStore::Observer final : public Node::Observer {
 public:
  static void Register(NodeLinkStore* owner, Node* node) noexcept {
    auto& obs = owner->obs_;
    auto& ptr = obs[node];
    if (!ptr) ptr = std::make_unique<Observer>(owner, node);
  }
  Observer(NodeLinkStore* owner, Node* node) noexcept:
      Node::Observer(node), owner_(owner) {
  }

  void ObserveSockChange() noexcept override {
    auto& items = owner_->items_;

    auto itr  = items.begin();
    auto term = items.end();

    while (itr < term) {
      auto& in  = itr->in;
      auto& out = itr->out;

      if (in.node == target()) {
        in.sock = target()->in(in.name);
      }
      if (out.node == target()) {
        out.sock = target()->out(out.name);
      }

      if (!in.sock || !out.sock) {
        if (owner_->dead_listener_) {
          owner_->dead_listener_(*itr);
        }
        std::swap(*itr, *(--term));
      } else {
        ++itr;
      }
    }
    items.erase(term, items.end());
  }
  void ObserveDie() noexcept override {
    // copy params on the stack because `this` will be deleted before escaping
    // this func
    auto owner = owner_;
    auto node  = target();
    Node::Observer::ObserveDie();

    owner->obs_.erase(node);
  }

 private:
  NodeLinkStore* owner_;
};

NodeLinkStore::NodeLinkStore(std::vector<SockLink>&& items) noexcept :
    items_(std::move(items)) {
  for (auto link : items_) {
    Observer::Register(this, link.in.node);
    Observer::Register(this, link.out.node);
  }
}

std::vector<NodeLinkStore::SockLink> NodeLinkStore::DeserializeLinks(
    const msgpack::object& obj, const std::vector<Node*>& nodes) {
  if (obj.type != msgpack::type::ARRAY) throw msgpack::type_error();

  std::vector<SockLink> ret;
  ret.resize(obj.via.array.size);
  for (size_t i = 0; i < obj.via.array.size; ++i) {
    const auto& link_obj = obj.via.array.ptr[i];

    std::tuple<size_t, std::string, size_t, std::string> link_tup;
    if (!link_obj.convert_if_not_nil(link_tup)) continue;

    const auto in_node_idx = std::get<0>(link_tup);
    if (in_node_idx >= nodes.size()) {
      throw DeserializeException("node index overflow");
    }
    const auto out_node_idx = std::get<2>(link_tup);
    if (in_node_idx >= nodes.size()) {
      throw DeserializeException("node index overflow");
    }

    auto in_node  = nodes[in_node_idx];
    auto out_node = nodes[out_node_idx];

    const auto& in_name  = std::get<1>(link_tup);
    const auto& out_name = std::get<3>(link_tup);

    auto in_sock  = in_node->in(in_name);
    auto out_sock = out_node->out(out_name);

    if (in_sock) {
      ret[i].in = {in_sock};
    } else {
      ret[i].in = {in_node, in_name};
    }
    if (out_sock) {
      ret[i].out = {out_sock};
    } else {
      ret[i].out = {out_node, out_name};
    }
  }
  return ret;
}
NodeLinkStore::NodeLinkStore(
    const msgpack::object& obj, const std::vector<Node*>& nodes)
try : NodeLinkStore(DeserializeLinks(obj, nodes)) {
} catch (msgpack::type_error&) {
  throw DeserializeException("broken NodeLinkStore");
}
void NodeLinkStore::Serialize(
    Packer& pk, const std::unordered_map<Node*, size_t>& idxmap) const noexcept {
  pk.pack_array(static_cast<uint32_t>(items_.size()));
  for (const auto& link : items_) {
    auto in_itr = idxmap.find(link.in.node);
    if (in_itr == idxmap.end()) continue;

    auto out_itr = idxmap.find(link.out.node);
    if (out_itr == idxmap.end()) continue;

    pk.pack(std::make_tuple(in_itr->second, link.in.name,
                            out_itr->second, link.out.name));
  }
}
std::unique_ptr<NodeLinkStore> NodeLinkStore::Clone(
    const std::unordered_map<Node*, Node*>& src_to_dst) const noexcept {
  std::vector<SockLink> ret;

  ret.reserve(items_.size());
  for (const auto& link : items_) {
    auto in_node_itr = src_to_dst.find(link.in.node);
    if (in_node_itr == src_to_dst.end()) continue;

    auto out_node_itr = src_to_dst.find(link.out.node);
    if (out_node_itr == src_to_dst.end()) continue;

    auto in_node  = in_node_itr->second;
    auto out_node = out_node_itr->second;

    auto in_sock  = in_node->in(link.in.name);
    auto out_sock = out_node->out(link.out.name);
    if (!in_sock || !out_sock) continue;

    ret.emplace_back(in_sock, out_sock);
  }
  return std::unique_ptr<NodeLinkStore>(new NodeLinkStore(std::move(ret)));
}

void NodeLinkStore::Link(InSock* in, OutSock* out) noexcept {
  items_.emplace_back(in, out);
  Observer::Register(this, in->owner());
  Observer::Register(this, out->owner());
}
void NodeLinkStore::Unlink(const InSock* in, const OutSock* out) noexcept {
  auto term = std::remove_if(items_.begin(), items_.end(),
                             [in, out](auto& x) { return x.in.sock == in && x.out.sock == out; });
  items_.erase(term, items_.end());
}

std::vector<NodeLinkStore::OutSock*> NodeLinkStore::GetSrcOf(const InSock* sock) const noexcept {
  std::vector<OutSock*> ret;
  for (const auto& link : items_) {
    if (link.in.sock == sock) {
      const auto& out = link.out;
      ret.push_back(out.node->out(out.name));
    }
  }
  return ret;
}
std::vector<NodeLinkStore::InSock*> NodeLinkStore::GetDstOf(const OutSock* sock) const noexcept {
  std::vector<InSock*> ret;
  for (const auto& link : items_) {
    if (link.out.sock == sock) {
      const auto& in = link.in;
      ret.push_back(in.node->in(in.name));
    }
  }
  return ret;
}

}  // namespace kingtaker
