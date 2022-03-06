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

  // Called when the user saves the project.
  virtual void OnSaved(File::RefStack&) noexcept { }

  // Called when the root window is closing.
  // Returning false can prevent it from being closed.
  virtual bool OnClosing(File::RefStack&) noexcept { return true; }

  // Called when the root window is closed.
  virtual void OnClosed(File::RefStack&) noexcept { }
};

}  // namespace kingtaker::iface
