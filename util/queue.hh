#pragma once

#include "kingtaker.hh"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <utility>


namespace kingtaker {

class SimpleQueue : public Queue {
 public:
  struct Item final {
    size_t                    id;
    Task                      task;
    std::chrono::milliseconds emit_on;

    constexpr bool operator>(const Item& other) const noexcept {
      return emit_on == other.emit_on? id > other.id: emit_on > other.emit_on;
    }
  };

  static std::chrono::milliseconds GetSystemTick() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
  }

  SimpleQueue() = default;

  void Push(Task&& t, std::chrono::milliseconds delay = 0ms) noexcept override {
    std::unique_lock<std::mutex> _(mtx_);
    q_.push({count_++, std::move(t), t_+delay});
    if (delay == 0ms) cv_.notify_all();
  }
  bool Pop() {
    std::unique_lock<std::mutex> k(mtx_);
    if (!pending()) return false;

    // take the task but not execute it yet because
    // it must be popped even if it throws an exception
    auto task = std::move(q_.top().task);
    q_.pop();

    k.unlock();
    task();
    return true;
  }

  void Tick(std::chrono::milliseconds t) noexcept {
    std::unique_lock<std::mutex> k(mtx_);
    t_ = t;
    cv_.notify_all();
  }
  void Wait() noexcept {
    std::unique_lock<std::mutex> k(mtx_);
    cv_.wait(k);
  }
  void Wake() noexcept {
    cv_.notify_all();
  }

 private:
  bool pending() const noexcept { return !q_.empty() && q_.top().emit_on <= t_; }

  std::mutex mtx_;

  size_t count_ = 0;

  std::chrono::milliseconds t_;

  std::condition_variable cv_;

  std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q_;
};


class CpuQueue : public SimpleQueue {
 public:
  CpuQueue() = delete;
  CpuQueue(size_t n = 2) noexcept : th_(n) {
    for (auto& t : th_) t = std::thread([this] () { Main(); });
  }

  ~CpuQueue() noexcept {
    alive_ = false;
    Wake();
    for (auto& t : th_) t.join();
  }

 private:
  void Main() noexcept {
    while (alive_) {
      while (Pop());
      Wait();
    }
  }


  std::atomic<bool> alive_ = true;

  std::vector<std::thread> th_;
};

}  // namespace kingtaker

