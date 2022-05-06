#pragma once

#include <cassert>
#include <memory>
#include <utility>

#include "iface/memento.hh"

#include "util/history.hh"


namespace kingtaker {

template <typename Owner, typename Data>
class SimpleMemento : public iface::Memento {
 public:
  SimpleMemento() = delete;
  SimpleMemento(Owner* owner, Data&& data) noexcept :
      owner_(owner), data_(Data(data)) {
    CommitForcibly();
  }

  void Commit() noexcept {
    if (!observed()) return;
    CommitForcibly();
  }
  void CommitForcibly() noexcept {
    tag_       = std::make_shared<WrappedTag>(this, Data(data_));
    tag_->self = tag_;
    iface::Memento::Commit(tag_);
  }
  void Overwrite() noexcept {
    tag_->data() = data_;
  }

  const Data& data() const noexcept { return data_; }
  Data& data() noexcept { return data_; }

  const Data& commitData() const noexcept { return tag_->data(); }

 private:
  Owner* owner_;
  Data   data_;

  class WrappedTag;
  std::shared_ptr<WrappedTag> tag_;


  class WrappedTag : public Tag {
   public:
    WrappedTag() = delete;
    WrappedTag(SimpleMemento* o, Data&& d) noexcept :
        owner_(o), data_(std::move(d)) {
    }

    void Restore() noexcept override {
      owner_->tag_  = self.lock();
      owner_->data_ = data_;
      owner_->data_.Restore(owner_->owner_);
    }

    Data& data() noexcept { return data_; }
    const Data& data() const noexcept { return data_; }

    std::weak_ptr<WrappedTag> self;

   private:
    SimpleMemento* owner_;

    Data data_;
  };
};

}  // namespace kingtaker
