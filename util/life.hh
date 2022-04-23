#pragma once

#include <atomic>
#include <memory>


namespace kingtaker {

class Life final {
 public:
  class Ref;

  Life() noexcept : alive_(std::make_shared<std::atomic<bool>>(true)) {
  }
  ~Life() noexcept {
    *alive_ = false;
  }
  Life(const Life&) = delete;
  Life(Life&&) = delete;
  Life& operator=(const Life&) = delete;
  Life& operator=(Life&&) = delete;

 private:
  std::shared_ptr<std::atomic<bool>> alive_;
};

class Life::Ref final {
 public:
  Ref() = delete;
  Ref(const Life& life) noexcept : alive_(life.alive_) {
  }
  Ref(const Ref&) = default;
  Ref(Ref&&) = default;
  Ref& operator=(const Ref&) = default;
  Ref& operator=(Ref&&) = default;

  bool operator*() const noexcept { return alive_ && *alive_; }

 private:
  std::shared_ptr<std::atomic<bool>> alive_;
};

}  // namespace kingtaker
