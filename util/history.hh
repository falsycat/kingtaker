#pragma once

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace kingtaker {

class HistoryCommand {
 public:
  HistoryCommand() = default;
  virtual ~HistoryCommand() = default;
  HistoryCommand(const HistoryCommand&) = delete;
  HistoryCommand(HistoryCommand&&) = delete;
  HistoryCommand& operator=(const HistoryCommand&) = delete;
  HistoryCommand& operator=(HistoryCommand&&) = delete;

  virtual void Apply()  = 0;
  virtual void Revert() = 0;
};

class HistoryAggregateCommand : public HistoryCommand {
 public:
  HistoryAggregateCommand() = delete;
  HistoryAggregateCommand(std::vector<std::unique_ptr<HistoryCommand>>&& cmds) noexcept :
      cmds_(std::move(cmds)) {
  }

  void Apply() override {
    for (const auto& cmd : cmds_) cmd->Apply();
  }
  void Revert() override {
    for (auto itr = cmds_.rbegin(); itr < cmds_.rend(); ++itr) {
      (*itr)->Revert();
    }
  }

 private:
  std::vector<std::unique_ptr<HistoryCommand>> cmds_;
};

// destructor must be called from a main task
template <typename T = HistoryCommand>
class History {
 public:
  using CommandList = std::vector<std::unique_ptr<T>>;

  History(CommandList&& cmds = {}, size_t cur = 0) :
      cmds_(std::move(cmds)), cursor_(cur) {
  }
  History(const History&) = delete;
  History(History&&) = delete;
  History& operator=(const History&) = delete;
  History& operator=(History&&) = delete;

  void UnDo() noexcept {
    auto task = [this]() {
      if (cursor_ == 0) return;
      cmds_[--cursor_]->Revert();
    };
    Queue::main().Push(std::move(task));
  }
  void ReDo() noexcept {
    auto task = [this]() {
      if (cursor_ >= cmds_.size()) return;
      cmds_[cursor_++]->Apply();
    };
    Queue::main().Push(std::move(task));
  }

  void AddSilently(std::unique_ptr<T>&& cmd) noexcept {
    cmds_.erase(cmds_.begin()+static_cast<intmax_t>(cursor_), cmds_.end());
    cmds_.push_back(std::move(cmd));
    ++cursor_;
  }
  void Queue(std::unique_ptr<T>&& cmd) noexcept {
    auto ptr = cmd.get();
    AddSilently(std::move(cmd));
    Queue::main().Push([ptr]() { ptr->Apply(); });
  }

  void Drop(size_t dist) noexcept {
    auto task = [this, dist]() {
      const size_t beg = cursor_ < dist? 0: cursor_-dist;
      const size_t end = std::min(cursor_+dist, cmds_.size());
      cmds_.erase(cmds_.begin()+end, cmds_.end());
      cmds_.erase(cmds_.begin(), cmds_.begin()+beg);
      cursor_ -= beg;
    };
    Queue::main().Push(std::move(task));
  }
  void Clear() noexcept {
    auto task = [this]() {
      cmds_.clear();
      cursor_ = 0;
    };
    Queue::main().Push(std::move(task));
  }

  const T& item(size_t idx) const noexcept { return *cmds_[idx]; }
  size_t cursor() const noexcept { return cursor_; }
  size_t size() const noexcept { return cmds_.size(); }

 private:
  CommandList cmds_;

  size_t cursor_;
};

}  // namespace kingtaker
