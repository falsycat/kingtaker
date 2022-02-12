#pragma once

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace kingtaker::iface {

class History {
 public:
  class Command;

  History() = default;
  virtual ~History() = default;
  History(const History&) = delete;
  History(History&&) = delete;
  History& operator=(const History&) = delete;
  History& operator=(History&&) = delete;

  virtual size_t Move(int32_t) noexcept = 0;

  virtual const Command& item(size_t) const noexcept = 0;

  virtual size_t cursor() const noexcept = 0;
  virtual size_t size() const noexcept = 0;
};

class History::Command {
 public:
  Command() = default;
  virtual ~Command() = default;
  Command(const Command&) = delete;
  Command(Command&&) = delete;
  Command& operator=(const Command&) = delete;
  Command& operator=(Command&&) = delete;

  virtual void Apply() = 0;
  virtual void Revert() = 0;
};


template <typename T = History::Command>
class SimpleHistory : public History {
 public:
  using CommandList = std::vector<std::unique_ptr<T>>;

  SimpleHistory(CommandList&& cmds = {}, size_t cur = 0) :
      cmds_(std::move(cmds)), cursor_(cur) {
  }
  SimpleHistory(const SimpleHistory&) = delete;
  SimpleHistory(SimpleHistory&&) = delete;
  SimpleHistory& operator=(const SimpleHistory&) = delete;
  SimpleHistory& operator=(SimpleHistory&&) = delete;

  size_t Move(int32_t step) noexcept {
    size_t ret = 0;
    if (step < 0) {
      const size_t dist = std::min(cursor_, static_cast<size_t>(-step));
      try {
        for (; ret < dist; ++ret) {
          cmds_[--cursor_]->Revert();
        }
      } catch (Exception&) {
      }
    }
    if (step > 0) {
      const size_t dist = std::min(static_cast<size_t>(step), cmds_.size()-cursor_);
      try {
        for (; ret < dist; ++ret) {
          cmds_[cursor_++]->Apply();
        }
      } catch (Exception&) {
      }
    }
    return ret;
  }

  void Add(std::unique_ptr<T>&& cmd) {
    cmd->Apply();

    cmds_.erase(cmds_.begin()+static_cast<intmax_t>(cursor_), cmds_.end());
    cmds_.push_back(std::move(cmd));
    ++cursor_;
  }
  void Queue(std::unique_ptr<T>&& cmd) noexcept {
    Queue::main().Push([this, ptr = cmd.release()]() { Add(std::unique_ptr<T>(ptr)); });
  }

  void Drop(size_t dist) noexcept {
    const size_t beg = cursor_ < dist? 0: cursor_-dist;
    const size_t end = std::min(cursor_+dist, cmds_.size());
    cmds_.erase(cmds_.begin()+end, cmds_.end());
    cmds_.erase(cmds_.begin(), cmds_.begin()+beg);
  }
  void Clear() noexcept {
    cmds_.clear();
    cursor_ = 0;
  }

  const T& item(size_t idx) const noexcept { return *cmds_[idx]; }
  size_t cursor() const noexcept { return cursor_; }
  size_t size() const noexcept { return cmds_.size(); }

 private:
  CommandList cmds_;

  size_t cursor_;
};

}  // namespace kingtaker::iface
