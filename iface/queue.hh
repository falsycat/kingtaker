#pragma once

#include "kingtaker.hh"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>


namespace kingtaker::iface {

// All methods are thread-safe.
class Queue {
 public:
  using Task = std::function<void()>;

  static Queue& Find(const File::RefStack& ref, std::string_view name) {
    const auto p = ref.ResolveUpward(File::Path {"_queue", std::string(name)});

    auto q = File::iface<Queue>(p.top().file());
    if (!q) throw File::UnsupportedException(p, "Queue");
    return *q;
  }

  Queue() = default;
  virtual ~Queue() = default;
  Queue(const Queue&) = delete;
  Queue(Queue&&) = delete;
  Queue& operator=(const Queue&) = delete;
  Queue& operator=(Queue&&) = delete;

  virtual void Push(Task&&, std::string_view = "") noexcept = 0;
};

class SimpleQueue : public Queue {
 public:
  struct Item {
    Task        task;
    std::string msg;
  };

  SimpleQueue() = default;

  void Push(Task&& t, std::string_view msg) noexcept override {
    std::unique_lock<std::mutex> _(mtx_);
    q_.emplace_back(std::move(t), std::string(msg));
    cv_.notify_all();
  }
  bool Pop(Item& item) noexcept {
    std::unique_lock<std::mutex> _(mtx_);
    if (q_.empty()) return false;

    item = std::move(q_.front());
    q_.pop_front();
    return true;
  }

  void Wait() {
    std::unique_lock<std::mutex> k(mtx_);
    cv_.wait(k);
  }

 protected:
  std::mutex mtx_;

  std::condition_variable cv_;

  std::deque<Item> q_;
};

}  // namespace kingtaker::iface
