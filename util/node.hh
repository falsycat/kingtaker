#pragma once

#include "kingtaker.hh"

#include "iface/node.hh"

#include "util/ptr_selector.hh"


namespace kingtaker {

// Use with LambdaNode<Driver>.
class LambdaNodeDriver : public iface::Node::Context::Data {
 public:
  using TypeInfo = File::TypeInfo;
  using RefStack = File::RefStack;
  using Packer   = File::Packer;
  using Node     = iface::Node;
  using Context  = iface::Node::Context;

  // static constexpr char* kTitle = "";

  enum SockFlag : uint8_t {
    kNone        = 0,
    kPulseButton = 1 << 0,
    kFrameHeight = 1 << 1,
  };
  using SockFlags = uint8_t;

  struct SockMeta final {
   public:
    SockMeta(const std::string& n, const std::string& d, SockFlags f = kNone) noexcept :
        name(n), desc(d), flags(f) {
    }
    std::string name;
    std::string desc;
    SockFlags   flags;
  };
  // static inline const std::vector<SockMeta> kInSocks;
  // static inline const std::vector<SockMeta> kOutSocks;

  LambdaNodeDriver() = default;
  // LambdaNodeDriver(LambdaNode*, const std::weak_ptr<Context>&);
  LambdaNodeDriver(const LambdaNodeDriver&) = default;
  LambdaNodeDriver(LambdaNodeDriver&&) = default;
  LambdaNodeDriver& operator=(const LambdaNodeDriver&) = default;
  LambdaNodeDriver& operator=(LambdaNodeDriver&&) = default;

  // void Handle(size_t idx, Value&&);

 protected:
  void Clear() noexcept {
    in_.clear();
  }
  void Set(size_t idx, Value&& v) noexcept {
    in_[idx] = std::move(v);
  }

  const Value& in(size_t idx) const noexcept {
    auto itr = in_.find(idx);
    if (itr == in_.end()) {
      static const Value pulse;
      return pulse;
    }
    return itr->second;
  }

 private:
  std::unordered_map<size_t, Value> in_;
};

template <typename Driver>
class LambdaNode final : public File, public iface::Node {
 public:
  LambdaNode(const std::shared_ptr<Env>& env) noexcept :
      File(&Driver::type_, env), Node(Node::kNone),
      life_(std::make_shared<std::monostate>()) {
    for (size_t i = 0; i < Driver::kOutSocks.size(); ++i) {
      const auto& m = Driver::kOutSocks[i];
      out_.emplace_back(new OutSock(this, m.name));
    }

    std::weak_ptr<std::monostate> life = life_;
    for (size_t i = 0; i < Driver::kInSocks.size(); ++i) {
      const auto& m = Driver::kInSocks[i];

      auto task = [this, life, i](auto& ctx, auto&& v) {
        if (life.expired()) return;
        GetDriver(ctx)->Handle(i, std::move(v));
      };
      in_.emplace_back(new LambdaInSock(this, m.name, std::move(task)));
    }
  }

  static std::unique_ptr<File> Deserialize(
      const msgpack::object&, const std::shared_ptr<Env>& env) noexcept {
    return std::make_unique<LambdaNode>(env);
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_nil();
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<LambdaNode>(env);
  }

  void Update(RefStack& ref, Event&) noexcept override {
    path_ = ref.GetFullPath();
  }
  void Update(RefStack&, const std::shared_ptr<Context>&) noexcept override;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node>(t).Select(this);
  }

  const Path& path() const noexcept { return path_; }

 private:
  std::shared_ptr<std::monostate> life_;

  Path path_;


  std::shared_ptr<Driver> GetDriver(const std::shared_ptr<Context>& ctx) noexcept {
    return ctx->GetOrNew<Driver>(this, this, std::weak_ptr<Context>(ctx));
  }
};
template <typename Driver>
void LambdaNode<Driver>::Update(RefStack&, const std::shared_ptr<Context>& ctx) noexcept {
  ImGui::TextUnformatted(Driver::kTitle);

  ImGui::BeginGroup();
  for (size_t i = 0; i < Driver::kInSocks.size(); ++i) {
    const auto& m = Driver::kInSocks[i];
    if (ImNodes::BeginInputSlot(m.name.c_str(), 1)) {
      const bool fh = m.flags & LambdaNodeDriver::kFrameHeight;

      if (m.flags & LambdaNodeDriver::kPulseButton) {
        gui::NodeInSock(ctx, in_[i], !fh /* = small */);
      } else {
        if (fh) ImGui::AlignTextToFramePadding();
        gui::NodeInSock(m.name);
      }
      ImNodes::EndSlot();

      if (m.desc.size() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", m.desc.c_str());
      }
    }
  }
  ImGui::EndGroup();

  ImGui::SameLine();

  ImGui::BeginGroup();
  float w = 0;
  for (const auto& m : Driver::kOutSocks) {
    w = std::max(w, ImGui::CalcTextSize(m.name.c_str()).x);
  }
  const auto left = ImGui::GetCursorPosX();
  for (const auto& m : Driver::kOutSocks) {
    ImGui::SetCursorPosX(left+w-ImGui::CalcTextSize(m.name.c_str()).x);
    if (ImNodes::BeginOutputSlot(m.name.c_str(), 1)) {
      gui::NodeOutSock(m.name);
      ImNodes::EndSlot();
      if (m.desc.size() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", m.desc.c_str());
      }
    }
  }
  ImGui::EndGroup();
}

}  // namespace kingtaker
