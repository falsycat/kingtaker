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

#include "util/notify.hh"
#include "util/queue.hh"

// To prevent conflicts because of fucking windows.h, include GLFW last.
#include <GLFW/glfw3.h>


#if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
# pragma comment(lib, "legacy_stdio_definitions")
#endif

using namespace std::literals;
using namespace kingtaker;

static constexpr const char* kFileName      = "kingtaker.bin";
static constexpr size_t      kTasksPerFrame = 1000;


static std::optional<std::string> panic_;

static SimpleQueue mainq_;
static SimpleQueue subq_;
static CpuQueue    cpuq_(2);
Queue& Queue::main() noexcept { return mainq_; }
Queue& Queue::sub() noexcept { return subq_; }
Queue& Queue::cpu() noexcept { return cpuq_; }

static std::unique_ptr<File> root_;
File& File::root() noexcept { return *root_; }

struct {
  File::Event::Status       st = File::Event::kNone;
  std::unordered_set<File*> focus;
} next_;


class Event final : public File::Event {
 public:
  Event() noexcept : File::Event(next_.st, std::move(next_.focus)) {
    next_.st = closing()? kClosed: kNone;
  }
  void CancelClosing(File* f, std::string_view msg) noexcept override {
    next_.st &= static_cast<Status>(~kClosed);
    notify::Warn(
        {}, f, "closing is refused by a file\nreason: "+std::string(msg));
  }
  void Focus(File* f) noexcept override {
    next_.focus.insert(f);
  }
};


void InitKingtaker() noexcept;
void Save()          noexcept;
void Update()        noexcept;
void UpdatePanic()   noexcept;
void UpdateAppMenu() noexcept;


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

  // main loop
  bool alive = true;
  while (alive) {
    if (next_.st & File::Event::kClosed) alive = false;
    glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    Update();
    ImGui::Render();

    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    glViewport(0, 0, w, h);

    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }

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
  const auto config = std::filesystem::current_path() / kFileName;

  auto env = std::make_shared<File::Env>(config, File::Env::kRoot);
  if (!std::filesystem::exists(kFileName)) {
    static const uint8_t kInitialRoot[] = {
#     include "generated/kingtaker.inc"
    };
    msgpack::object_handle obj =
        msgpack::unpack(reinterpret_cast<const char*>(kInitialRoot),
                        sizeof(kInitialRoot));
    root_ = File::Deserialize(obj.get(), env);
    assert(root_);
    return;
  }

  try {
    // open the file
    std::ifstream st(kFileName, std::ios::binary);
    if (!st) {
      throw DeserializeException("failed to open: "s+config.string());
    }
    root_ = File::Deserialize(st, env);

  } catch (msgpack::unpack_error& e) {
    panic_ = "MessagePack unpack error: "s+e.what();

  } catch (DeserializeException& e) {
    panic_ = e.Stringify();
  }
}

void Save() noexcept {
  next_.st |= File::Event::kSaved;

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
    UpdatePanic();
    return;
  }

  // tick task queues
  const auto q_tick = SimpleQueue::GetSystemTick();
  mainq_.Tick(q_tick);
  subq_ .Tick(q_tick);
  cpuq_ .Tick(q_tick);

  // update GUI
  File::RefStack path;
  Event          ev;
  root_->Update(path, ev);
  UpdateAppMenu();

  // execute tasks
  try {
    size_t done = 0;
    while (mainq_.Pop()) ++done;
    while (done < kTasksPerFrame && subq_.Pop()) ++done;
  } catch (Exception& e) {
    panic_ = e.Stringify();
  }
}
void UpdatePanic() noexcept {
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
}
void UpdateAppMenu() noexcept {
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("App")) {
      if (ImGui::MenuItem("Save")) {
        mainq_.Push(Save);
      }
      if (ImGui::MenuItem("Quit")) {
        next_.st = File::Event::kClosing;
      }
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
  }
}
