#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <thread>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>

#include "kingtaker.hh"

#include "util/queue.hh"

#include "iface/gui.hh"

// To prevent conflicts because of fucking windows.h, include GLFW last.
#include <GLFW/glfw3.h>


#if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
# pragma comment(lib, "legacy_stdio_definitions")
#endif

using namespace std::literals;
using namespace kingtaker;


static const char*      kFileName      = "kingtaker.bin";
static constexpr size_t kTasksPerFrame = 1000;


static std::optional<std::string> panic_;
static bool                       alive_;

static SimpleQueue mainq_;
static SimpleQueue subq_;
static CpuQueue    cpuq_(2);
Queue& Queue::main() noexcept { return mainq_; }
Queue& Queue::sub() noexcept { return subq_; }
Queue& Queue::cpu() noexcept { return cpuq_; }

static std::unique_ptr<File> root_;
File& File::root() noexcept { return *root_; }


void InitKingtaker() noexcept;
void Save() noexcept;
void Update() noexcept;


int main(int, char**) {
  // init display
  glfwSetErrorCallback(
      [](int, const char* msg) {
        std::cout << "GLFW error: " << msg << std::endl;
      });
  if (!glfwInit()) return 1;

  GLFWwindow* window;
  const char* glsl_version;
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
# if defined(__APPLE__)
    glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
# else
    glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
# endif
  window = glfwCreateWindow(1280, 720, "KINGTAKER", NULL, NULL);
  if (window == NULL) return 1;
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  // init ImGUI
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImPlot::CreateContext();

  auto& io = ImGui::GetIO();
  io.IniFilename  = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);

  // init kingtaker
  InitKingtaker();

  glfwShowWindow(window);
  alive_ = true;

  // main loop
  while (alive_) {
    glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    Update();
    if (!alive_) {
      // call OnClosing event
      File::RefStack path;
      alive_ = !File::iface(*root_, iface::GUI::null()).OnClosing(path);
    }
    ImGui::Render();

    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    glViewport(0, 0, w, h);

    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }

  // call OnClosed event
  File::RefStack path;
  File::iface(*root_, iface::GUI::null()).OnClosed(path);

  // teardown ImGUI
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImPlot::DestroyContext();
  ImGui::DestroyContext();

  // teardown display
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}


void InitKingtaker() noexcept {
  if (!std::filesystem::exists(kFileName)) {
    static const uint8_t kInitialRoot[] = {
#     include "generated/kingtaker.inc"
    };
    msgpack::object_handle obj =
        msgpack::unpack(reinterpret_cast<const char*>(kInitialRoot),
                        sizeof(kInitialRoot));
    root_ = File::Deserialize(obj.get());
    assert(root_);
    return;
  }

  try {
    // open the file
    std::ifstream st(kFileName, std::ios::binary);
    if (!st) {
      throw DeserializeException("failed to open: "s+kFileName);
    }
    root_ = File::Deserialize(st);

  } catch (msgpack::unpack_error& e) {
    panic_ = "MessagePack unpack error: "s+e.what();

  } catch (DeserializeException& e) {
    panic_ = e.Stringify();
  }
}

void Save() noexcept {
  File::RefStack path;
  File::iface(*root_, iface::GUI::null()).OnSaved(path);

  std::ofstream f(kFileName, std::ios::binary);
  if (!f) {
    panic_ = "failed to open: "s+kFileName;
    return;
  }

  msgpack::packer<std::ostream> pk(f);
  root_->SerializeWithTypeInfo(pk);
  if (!f) {
    panic_ = "failed to write: "s+kFileName;
    return;
  }
}

void Update() noexcept {
  // panic msg
  if (panic_) {
    static const char*    kWinId    = "PANIC##kingtaker/main.cc";
    static constexpr auto kWinFlags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;

    static constexpr auto kWidth  = 32;  /* em */
    static constexpr auto kHeight = 8;   /* em */

    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowContentSize({kWidth*em, 0});

    if (ImGui::BeginPopupModal(kWinId, nullptr, kWinFlags)) {
      ImGui::Text("### something went wrong X( ###");
      ImGui::InputTextMultiline(
          "##message",
          panic_->data(), panic_->size(),
          ImVec2 {kWidth*em, kHeight*em},
          ImGuiInputTextFlags_ReadOnly);

      if (ImGui::Button("IGNORE")) {
        panic_ = std::nullopt;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("ABORT")) {
        std::abort();  // immediate death
      }
      ImGui::EndPopup();

    } else if (panic_) {
      ImGui::OpenPopup(kWinId);
    }
    return;
  }

  // tick task queues
  const auto q_tick = SimpleQueue::GetSystemTick();
  mainq_.Tick(q_tick);
  subq_ .Tick(q_tick);
  cpuq_ .Tick(q_tick);

  // application menu
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("App")) {
      if (ImGui::MenuItem("Save")) {
        mainq_.Push(Save);
      }
      if (ImGui::MenuItem("Quit")) {
        alive_ = false;
      }
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
  }

  // update GUI
  auto gui = root_->iface<iface::GUI>();
  if (!gui) {
    panic_ = "ROOT doesn't have a GUI interface X(";
    return;
  }
  File::RefStack path;
  gui->Update(path);

  // execute tasks
  try {
    size_t done = 0;
    while (mainq_.Pop()) ++done;
    while (done < kTasksPerFrame && subq_.Pop()) ++done;
  } catch (Exception& e) {
    panic_ = e.Stringify();
  }
}
