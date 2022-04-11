#pragma once

#include <unordered_map>

#include "iface/memento.hh"


namespace kingtaker {

template <typename T>
class SimpleMemento : public iface::Memento {
 public:
  SimpleMemento(T&& data) noexcept :
      data_(T(data)),
      tag_(std::make_shared<WrappedTag>(this, std::move(data))) {
    tags_[tag_.get()] = tag_;
  }
  ~SimpleMemento() noexcept {
    tag_ = nullptr;
    CleanUp();
    assert(tags_.size() == 0 && "all tags should be deleted before file deletion");
  }
  SimpleMemento(const SimpleMemento&) = delete;
  SimpleMemento(SimpleMemento&&) = delete;
  SimpleMemento& operator=(const SimpleMemento&) = delete;
  SimpleMemento& operator=(SimpleMemento&&) = delete;

  std::shared_ptr<Tag> Save() noexcept override {
    if (!committed_) return tag_;
    committed_ = false;

    tag_ = std::make_shared<WrappedTag>(this, T(data_));
    tags_[tag_.get()] = tag_;
    return tag_;
  }
  void Commit() noexcept {
    committed_ = true;
  }

  const T& data() const noexcept { return data_; }
  T& data() noexcept { return data_; }

  const T& commitData() const noexcept { return tag_->data(); }

 private:
  T data_;

  class WrappedTag;
  std::shared_ptr<WrappedTag> tag_;

  bool committed_ = false;

  std::unordered_map<WrappedTag*, std::weak_ptr<WrappedTag>> tags_;


  void CleanUp() noexcept {
    for (auto itr = tags_.begin(); itr != tags_.end();) {
      if (itr->second.expired()) {
        itr = tags_.erase(itr);
      } else {
        ++itr;
      }
    }
  }
  std::shared_ptr<WrappedTag> Upgrade(WrappedTag* t) const noexcept {
    auto itr = tags_.find(t);
    if (itr != tags_.end()) {
      return itr->second.lock();
    }
    assert(false);
    return nullptr;
  }


  class WrappedTag : public Tag {
   public:
    WrappedTag() = delete;
    WrappedTag(SimpleMemento* o, T&& d) noexcept :
        owner_(o), data_(std::move(d)) {
    }
    ~WrappedTag() noexcept {
      owner_->CleanUp();
    }

    std::shared_ptr<Tag> Restore() noexcept override {
      auto ret = owner_->tag_;
      owner_->tag_ = owner_->Upgrade(this);
      owner_->data_.Restore(data_);
      return ret;
    }

    const T& data() const noexcept { return data_; }

   private:
    SimpleMemento* owner_;

    T data_;
  };
};

}  // namespace kingtaker
