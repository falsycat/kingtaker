#pragma once

#include "kingtaker.hh"


namespace kingtaker::iface {

class GUI {
 public:
  enum Feature : uint8_t {
    kWindow  = 0b000001,
    kMenu    = 0b000010,
    kTree    = 0b000100,
    kEditor  = 0b001000,
    kTooltip = 0b010000,
    kNode    = 0b100000,
  };
  using Features = uint8_t;

  static GUI& null() noexcept { static GUI instance; return instance; }

  GUI(Features feats = 0) : feats_(feats) { }
  virtual ~GUI() = default;
  GUI(const GUI&) = delete;
  GUI(GUI&&) = delete;
  GUI& operator=(const GUI&) = delete;
  GUI& operator=(GUI&&) = delete;

  virtual void UpdateWindow(File::RefStack&) noexcept { }
  virtual void UpdateMenu(File::RefStack&) noexcept { }
  virtual void UpdateTree(File::RefStack&) noexcept { }
  virtual void UpdateEditor(File::RefStack&) noexcept { }
  virtual void UpdateTooltip(File::RefStack&) noexcept { }
  virtual void UpdateNode(File::RefStack&) noexcept { }

  Features feats() const noexcept { return feats_; }

 private:
  Features feats_;
};

}  // namespace kingtaker::iface
