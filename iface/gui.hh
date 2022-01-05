#pragma once

#include "kingtaker.hh"


namespace kingtaker::iface {

class GUI {
 public:
  static GUI& null() noexcept { static GUI instance; return instance; }

  GUI() = default;
  virtual ~GUI() = default;
  GUI(const GUI&) = delete;
  GUI(GUI&&) = delete;
  GUI& operator=(const GUI&) = delete;
  GUI& operator=(GUI&&) = delete;

  virtual void Update(File::RefStack&) noexcept { }
};

}  // namespace kingtaker::iface
