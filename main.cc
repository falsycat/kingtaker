#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>

#include <GL/glew.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>

#include "kingtaker.hh"

#include "util/gl.hh"
#include "util/gui.hh"
#include "util/queue.hh"

// To prevent conflicts because of fucking windows.h, include GLFW last.
#include <GLFW/glfw3.h>


#if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
# pragma comment(lib, "legacy_stdio_definitions")
#endif

using namespace std::literals;
using namespace kingtaker;

constexpr const char*           kFileName    = "kingtaker.bin";
constexpr size_t                kSubTaskUnit = 100;
constexpr std::chrono::duration kFrameDur    = 1000ms / 30;


static std::mutex              main_mtx_;
static std::condition_variable main_cv_;
static std::thread             main_worker_;
static std::atomic<bool>       main_alive_ = true;

static std::mutex  panic_mtx_;
static std::string panic_;

static SimpleQueue mainq_;
static SimpleQueue subq_;
static SimpleQueue glq_;
static CpuQueue    cpuq_(2);
Queue& Queue::main() noexcept { return mainq_; }
Queue& Queue::sub() noexcept { return subq_; }
Queue& Queue::cpu() noexcept { return cpuq_; }
Queue& Queue::gl() noexcept { return glq_; }

File::Env env_(std::filesystem::current_path(), File::Env::kRoot);

static std::unique_ptr<File> root_;
File& File::root() noexcept { return *root_; }

struct {
  File::Event::Status st = File::Event::kNone;
  std::unordered_set<File*> focus;
} next_;


class Event final : public File::Event {
 public:
  Event() noexcept : File::Event(next_.st, std::move(next_.focus)) {
    next_.st = closing()? kClosed: kNone;
  }
  void CancelClosing(File*, std::string_view) noexcept override {
    next_.st &= static_cast<Status>(~kClosed);
    // TODO: logging
  }
  void Focus(File* f) noexcept override {
    next_.focus.insert(f);
  }
};


void InitKingtaker() noexcept;

void Update()        noexcept;
bool UpdatePanic()   noexcept;
void UpdateAppMenu() noexcept;
void Save()          noexcept;

void Panic(const std::string&) noexcept;
std::string GenerateSystemInfoFullText() noexcept;

void WorkerMain() noexcept;


int main(int, char**) {
  // starts main worker
  main_alive_ = true;
  main_worker_ = std::thread(WorkerMain);

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
  if (glewInit() != GLEW_OK) return 1;

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
    const auto t = Clock::now();

    // new frame
    if (next_.st & File::Event::kClosed) alive = false;
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    {
      std::unique_lock<std::mutex> k(main_mtx_);
      main_cv_.wait(k, []() { return !mainq_.pending(); });
      Update();
      main_cv_.notify_one();
    }

    // render windows
    ImGui::Render();

    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    glViewport(0, 0, w, h);

    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);

    // handle GL queue
    const auto until = t + kFrameDur;
    do {
      try {
        size_t i = 0;
        while (i < kSubTaskUnit && (gl::HandleAll(), glq_.Pop())) ++i;
      } catch (gl::Exception& e) {
        Panic(e.Stringify());
      }
      glq_.WaitUntil(until);
    } while (Clock::now() < until);
  }
  // request main worker to exit
  main_alive_ = false;
  main_cv_.notify_one();

  // teardown ImGUI
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImPlot::DestroyContext();
  ImGui::DestroyContext();

  // teardown display
  glfwDestroyWindow(window);
  glfwTerminate();

  // teardown system
  main_worker_.join();
  root_ = nullptr;
  return 0;
}


void InitKingtaker() noexcept {
  const auto config = env_.npath() / kFileName;
  if (!std::filesystem::exists(config)) {
    static const uint8_t kInitialRoot[] = {
#     include "generated/kingtaker.inc"
    };
    const auto obj = msgpack::unpack(
        reinterpret_cast<const char*>(kInitialRoot), sizeof(kInitialRoot));
    root_ = File::Deserialize(&env_, obj.get());
    assert(root_);
    return;
  }

  try {
    std::ifstream st(config, std::ios::binary);
    if (!st) {
      throw DeserializeException("failed to open: "s+config.string());
    }
    root_ = File::Deserialize(&env_, st);

  } catch (msgpack::unpack_error& e) {
    Panic("MessagePack unpack error: "s+e.what());

  } catch (DeserializeException& e) {
    Panic(e.Stringify());
  }
}

void Update() noexcept {
  // panic msg
  if (UpdatePanic()) return;

  // update GUI
  File::RefStack path;
  Event          ev;
  root_->Update(path, ev);
  UpdateAppMenu();
}
bool UpdatePanic() noexcept {
  static const char*    kWinId    = "PANIC##kingtaker/main.cc";
  constexpr    auto     kWinFlags =
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;

  constexpr auto kWidth  = 32;  /* em */
  constexpr auto kHeight = 8;   /* em */

  std::unique_lock<std::mutex> k(panic_mtx_);
  if (panic_.empty()) return false;

  const float em = ImGui::GetFontSize();
  ImGui::SetNextWindowContentSize({kWidth*em, 0});

  if (ImGui::BeginPopupModal(kWinId, nullptr, kWinFlags)) {
    ImGui::Text("### something went wrong X( ###");
    ImGui::InputTextMultiline(
        "##message",
        panic_.data(), panic_.size(),
        ImVec2 {kWidth*em, kHeight*em},
        ImGuiInputTextFlags_ReadOnly);

    if (ImGui::Button("IGNORE")) {
      panic_ = "";
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("ABORT")) {
      std::abort();  // immediate death
    }
    ImGui::EndPopup();

  } else if (panic_.size()) {
    ImGui::OpenPopup(kWinId);
  }
  return true;
}
void UpdateAppMenu() noexcept {
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("App")) {
      if (ImGui::MenuItem("save")) {
        mainq_.Push(Save);
      }
      if (ImGui::MenuItem("quit")) {
        next_.st = File::Event::kClosing;
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
      if (ImGui::MenuItem("focus root")) {
        next_.focus.insert(&*File::RefStack());
      }
      if (ImGui::BeginMenu("focus by path")) {
        static std::string path, path_editing;
        File::RefStack ref;
        if (gui::InputPathMenu(ref, &path_editing, &path)) {
          next_.focus.insert(&*ref.Resolve(path));
        }
        ImGui::EndMenu();
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Info")) {
      if (ImGui::BeginMenu("registered types")) {
        for (const auto& p : File::registry()) {
          auto t = p.second;
          ImGui::MenuItem(t->name().c_str());
          if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("name   : %s", t->name().c_str());
            ImGui::Text("desc   : %s", t->desc().c_str());
            ImGui::Text("factory:");
            ImGui::Indent();
            if (t->factory())      { ImGui::Bullet(); ImGui::TextUnformatted("New"); }
            if (t->deserializer()) { ImGui::Bullet(); ImGui::TextUnformatted("Deserialize"); }
            ImGui::Unindent();
            ImGui::EndTooltip();
          }
        }
        ImGui::EndMenu();
      }

      ImGui::MenuItem("system");
      if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("KINGTAKER vX.Y.Z (WTFPL)");
        ImGui::TextUnformatted("no fee, no copyright, no limitation");
        ImGui::EndTooltip();
      }

      ImGui::Separator();
      if (ImGui::MenuItem("copy full info as text")) {
        ImGui::SetClipboardText(GenerateSystemInfoFullText().c_str());
      }
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
  }
}
void Save() noexcept {
  next_.st |= File::Event::kSaved;

  const auto config = env_.npath() / kFileName;

  std::ofstream f(config, std::ios::binary);
  if (!f) {
    Panic("failed to open: "s+kFileName);
    return;
  }

  msgpack::packer<std::ostream> pk(f);
  root_->SerializeWithTypeInfo(pk);
  if (!f) {
    Panic("failed to write: "s+kFileName);
    return;
  }
}

void Panic(const std::string& msg) noexcept {
  std::unique_lock<std::mutex> k(panic_mtx_);
  panic_ += msg + "\n\n####\n\n";
}
std::string GenerateSystemInfoFullText() noexcept {
  std::string ret =
      "# KINGTAKER vX.Y.Z (WTFPL)\n"
      "\n"
      "## REGISTRY\n";
  for (auto& p : File::registry()) {
    ret += "- "s+p.first+"\n";
  }
  return ret;
}

void WorkerMain() noexcept {
  std::unique_lock<std::mutex> k(main_mtx_);
  while (main_alive_) {
    if (!k) k.lock();

    // wait for a request of queuing or exiting
    main_cv_.wait(k, []() {
        return !main_alive_ || mainq_.pending() || subq_.pending();
      });

    try {
      // empty mainq_ firstly
      while (mainq_.Pop());
      main_cv_.notify_one();

      for (;;) {
        // executes some tasks
        size_t i = 0;
        while (i < kSubTaskUnit && subq_.Pop()) ++i;
        if (i < kSubTaskUnit || !main_alive_) break;

        // if relock fails or mainq is not empty, handle mainq again
        k.unlock();
        if (!k.try_lock() || mainq_.pending()) break;
      }

    } catch (Exception& e) {
      Panic(e.Stringify());
    }
  }
}
