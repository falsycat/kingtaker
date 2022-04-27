#pragma once

#include <cassert>
#include <memory>
#include <utility>

#include "iface/memento.hh"

#include "util/history.hh"


namespace kingtaker {

template <typename T>
class SimpleMemento : public iface::Memento {
 public:
  SimpleMemento() = delete;
  SimpleMemento(T&& data) noexcept : data_(T(data)) {
    CommitForcibly();
  }

  void Commit() noexcept {
    if (!observed()) return;
    CommitForcibly();
  }
  void CommitForcibly() noexcept {
    tag_       = std::make_shared<WrappedTag>(this, T(data_));
    tag_->self = tag_;
    iface::Memento::Commit(tag_);
  }
  void Overwrite() noexcept {
    tag_->data() = data_;
  }

  const T& data() const noexcept { return data_; }
  T& data() noexcept { return data_; }

  const T& commitData() const noexcept { return tag_->data(); }

 private:
  T data_;

  class WrappedTag;
  std::shared_ptr<WrappedTag> tag_;


  class WrappedTag : public Tag {
   public:
    WrappedTag() = delete;
    WrappedTag(SimpleMemento* o, T&& d) noexcept :
        owner_(o), data_(std::move(d)) {
    }

    void Restore() noexcept override {
      owner_->tag_ = self.lock();
      owner_->data_.Restore(data_);
    }

    T& data() noexcept { return data_; }
    const T& data() const noexcept { return data_; }

    std::weak_ptr<WrappedTag> self;

   private:
    SimpleMemento* owner_;

    T data_;
  };
};

}  // namespace kingtaker
