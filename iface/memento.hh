#pragma once

#include <memory>

#include "kingtaker.hh"


namespace kingtaker::iface {

class Memento {
 public:
  class Tag;
  class CollapseException;

  Memento() = default;
  virtual ~Memento() = default;
  Memento(const Memento&) = delete;
  Memento(Memento&&) = delete;
  Memento& operator=(const Memento&) = delete;
  Memento& operator=(Memento&&) = delete;

  virtual std::shared_ptr<Tag> Save() noexcept = 0;
};

class Memento::Tag {
 public:
  Tag() = default;
  virtual ~Tag() = default;
  Tag(const Tag&) = delete;
  Tag(Tag&&) = delete;
  Tag& operator=(const Tag&) = delete;
  Tag& operator=(Tag&&) = delete;

  // returns previous tag
  virtual std::shared_ptr<Tag> Restore() = 0;
};

class Memento::CollapseException : public Exception {
 public:
  CollapseException(std::string_view msg, Loc loc = Loc::current()) noexcept :
      Exception(msg, loc) {
  }
};

}  // namespace kingtaker::iface
