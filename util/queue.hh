#pragma once

#include "kingtaker.hh"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>


namespace kingtaker {

class SimpleQueue : public Queue {
 public:
  SimpleQueue() = default;

  void Push(Task&& t) noexcept override {
    std::unique_lock<std::mutex> _(mtx_);
    q_.push_back(std::move(t));
    ++cnt_;
    cv_.notify_all();
  }
  bool Pop() {
    std::unique_lock<std::mutex> k(mtx_);
    if (q_.empty()) return false;

    // take the task but not execute it yet because
    // it must be popped even if it throws an exception
    auto task = std::move(q_.front());
    q_.pop_front();
    --cnt_;

    k.unlock();
    task();
    return true;
  }

  void Wait() noexcept {
    std::unique_lock<std::mutex> k(mtx_);
    cv_.wait(k);
  }
  void Wake() noexcept {
    cv_.notify_all();
  }

  bool pending() const noexcept {
    return cnt_ > 0;
  }

 private:
  std::mutex mtx_;

  std::condition_variable cv_;

  std::atomic<size_t> cnt_;

  std::deque<Task> q_;
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

