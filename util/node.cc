#include "util/node.hh"

#include <unordered_set>

#include <msgpack.hh>


namespace kingtaker {

NodeLinkStore::NodeLinkStore(
    const msgpack::object& obj, const std::vector<Node*>& nodes) {
  if (obj.type != msgpack::type::MAP) throw msgpack::type_error();

  for (size_t i = 0; i < obj.via.map.size; ++i) {
    const auto& src_obj      = obj.via.map.ptr[i];
    const auto  src_node_idx = src_obj.key.as<size_t>();
    const auto& srcs_obj     = obj.via.map.ptr[i].val;
    if (srcs_obj.type != msgpack::type::MAP) throw msgpack::type_error();

    if (src_node_idx >= nodes.size()) continue;
    auto src_node = nodes[src_node_idx];

    for (size_t j = 0; j < srcs_obj.via.map.size; ++j) {
      const auto& src_obj  = srcs_obj.via.map.ptr[j];
      const auto  src_name = src_obj.key.as<std::string>();
      const auto& dsts_obj = src_obj.val;
      if (dsts_obj.type != msgpack::type::ARRAY) throw msgpack::type_error();

      auto src = src_node->out(src_name);
      if (!src) continue;

      for (size_t k = 0; k < dsts_obj.via.array.size; ++k) {
        const auto& dst_obj = dsts_obj.via.array.ptr[k];
        if (dst_obj.type != msgpack::type::ARRAY) throw msgpack::type_error();
        if (dst_obj.via.array.size != 2) throw msgpack::type_error();

        const auto dst_node_idx = dst_obj.via.array.ptr[0].as<size_t>();
        const auto dst_name     = dst_obj.via.array.ptr[1].as<std::string>();

        if (dst_node_idx >= nodes.size()) continue;
        auto dst_node = nodes[dst_node_idx];

        auto dst = dst_node->in(dst_name);
        if (!dst) continue;

        Link(dst, src);
      }
    }
  }
}
void NodeLinkStore::Serialize(
    Packer& pk, const std::unordered_map<Node*, size_t>& idxmap) const noexcept {
  std::unordered_set<Node*> nodes;
  for (const auto& p : out_) {
    nodes.insert(p.first->owner());
  }

  pk.pack_map(static_cast<uint32_t>(nodes.size()));
  for (auto n : nodes) {
    const auto idx_itr = idxmap.find(n);
    assert(idx_itr != idxmap.end());
    pk.pack(idx_itr->second);

    pk.pack_map(static_cast<uint32_t>(n->out().size()));
    for (const auto& s : n->out()) {
      pk.pack(s->name());

      const auto linkset_itr = out_.find(s.get());
      if (linkset_itr == out_.end()) {
        pk.pack_nil();
        continue;
      }
      const auto& others = linkset_itr->second.others();
      pk.pack_array(static_cast<uint32_t>(others.size()));

      for (const auto& other : others) {
        const auto other_idx_itr = idxmap.find(other->owner());
        assert(other_idx_itr != idxmap.end());
        pk.pack_array(2);
        pk.pack(other_idx_itr->second);
        pk.pack(other->name());
      }
    }
  }
}
NodeLinkStore NodeLinkStore::Clone(
    const std::unordered_map<Node*, Node*>& src_to_dst) const noexcept {
  NodeLinkStore ret;
  for (const auto& p : out_) {
    auto out_node_itr = src_to_dst.find(p.second.self()->owner());
    if (out_node_itr == src_to_dst.end()) continue;

    const auto& out_name = p.second.self()->name();
    auto out_node = out_node_itr->second;

    for (const auto& in : p.second.others()) {
      auto in_node_itr = src_to_dst.find(in->owner());
      if (in_node_itr == src_to_dst.end()) continue;

      auto in_node = in_node_itr->second;

      auto insock  = in_node->in(in->name());
      auto outsock = out_node->out(out_name);
      if (insock && outsock) ret.Link(insock, outsock);
    }
  }
  return ret;
}

void NodeLinkStore::Link(const std::shared_ptr<InSock>&  in,
                         const std::shared_ptr<OutSock>& out) noexcept {
  auto in_set = in_.try_emplace(in.get(), in).first;
  in_set->second.Link(out);

  auto out_set = out_.try_emplace(out.get(), out).first;
  out_set->second.Link(in);
}
void NodeLinkStore::Unlink(const InSock* in, const OutSock* out) noexcept {
  auto in_itr  = in_.find(const_cast<InSock*>(in));
  auto out_itr = out_.find(const_cast<OutSock*>(out));

  const bool in_found  = in_itr  != in_.end();
  const bool out_found = out_itr != out_.end();
  assert(in_found == out_found);
  if (!in_found && !out_found) return;

  if (in_itr->second.Unlink(*out)) {
    in_.erase(in_itr);
  }
  if (out_itr->second.Unlink(*in)) {
    out_.erase(out_itr);
  }
}
void NodeLinkStore::CleanUp() noexcept {
  std::vector<InSock*> in_dead;
  for (const auto& in : in_) {
    if (!in.second.alive()) in_dead.push_back(in.first);
  }
  for (const auto in : in_dead) Unlink(*in);

  std::vector<OutSock*> out_dead;
  for (const auto& out : out_) {
    if (!out.second.alive()) out_dead.push_back(out.first);
  }
  for (const auto out : out_dead) Unlink(*out);
}

}  // namespace kingtaker
