#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include "kingtaker.hh"


namespace kingtaker::iface {

class Memento {
 public:
  class Observer;

  class Tag;
  class CollapseException;

  Memento() = default;
  virtual inline ~Memento() noexcept;
  Memento(const Memento&) = delete;
  Memento(Memento&&) = delete;
  Memento& operator=(const Memento&) = delete;
  Memento& operator=(Memento&&) = delete;

  bool observed() const noexcept { return obs_.size() > 0; }

  const std::shared_ptr<Tag>& tag() const noexcept { return tag_; }

 protected:
  inline void Commit(const std::shared_ptr<Tag>&) noexcept;
  inline void NotifyRestore() noexcept;

 private:
  std::vector<Observer*> obs_;

  std::shared_ptr<Tag> tag_;
};

class Memento::Observer {
 public:
  Observer(Memento* target) noexcept : target_(target) {
    target_->obs_.push_back(this);
  }
  virtual ~Observer() noexcept {
    if (!target_) return;
    auto& obs = target_->obs_;
    auto  itr = std::find(obs.begin(), obs.end(), this);
    if (itr != obs.end()) obs.erase(itr);
  }
  Observer(const Observer&) = delete;
  Observer(Observer&&) = delete;
  Observer& operator=(const Observer&) = delete;
  Observer& operator=(Observer&&) = delete;

  virtual void ObserveCommit() noexcept { }
  virtual void ObserveDie() noexcept {
    target_ = nullptr;
  }

  Memento* target() const noexcept { return target_; }

 private:
  Memento* target_;
};

Memento::~Memento() noexcept {
  for (auto obs : obs_) obs->ObserveDie();
}
void Memento::Commit(const std::shared_ptr<Tag>& tag) noexcept {
  tag_ = tag;
  for (auto obs : obs_) obs->ObserveCommit();
}

class Memento::Tag {
 public:
  Tag() = default;
  virtual ~Tag() = default;
  Tag(const Tag&) = delete;
  Tag(Tag&&) = delete;
  Tag& operator=(const Tag&) = delete;
  Tag& operator=(Tag&&) = delete;

  virtual void Restore() = 0;
};

class Memento::CollapseException : public Exception {
 public:
  CollapseException(std::string_view msg, Loc loc = Loc::current()) noexcept :
      Exception(msg, loc) {
  }
};

}  // namespace kingtaker::iface
