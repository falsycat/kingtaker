#pragma once

namespace kingtaker::iface {

template <typename T>
class Factory {
 public:
  Factory() = default;
  virtual ~Factory() = default;
  Factory(const Factory&) = default;
  Factory(Factory&&) = default;
  Factory& operator=(const Factory&) = default;
  Factory& operator=(Factory&&) = default;

  virtual T Create() noexcept = 0;
};

}  // namespace kingtaker::iface
