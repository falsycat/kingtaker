#include "kingtaker.hh"

#include <atomic>
#include <thread>

#include <imgui.h>

#include "iface/queue.hh"


namespace kingtaker {

class SystemMainQueue : public File, public iface::Queue {
 public:
  static inline TypeInfo* type_ = TypeInfo::NewWithoutFactory<SystemMainQueue>(
      "SystemMainQueue", "a queue for primary tasks executed by main thread");

  SystemMainQueue() : File(type_) { }

  void Push(Task&& task, std::string_view msg) noexcept override {
    File::QueueMainTask(std::move(task), msg);
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object&) {
    return std::make_unique<SystemMainQueue>();
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_nil();
  }
  std::unique_ptr<File> Clone() const noexcept override {
    return std::make_unique<SystemMainQueue>();
  }

  Time lastModified() const noexcept override { return {}; }

  void* iface(const std::type_index& idx) noexcept override {
    if (idx == typeid(iface::Queue)) return static_cast<iface::Queue*>(this);
    return nullptr;
  }
};

class SystemSubQueue : public File, public iface::Queue {
 public:
  static inline TypeInfo* type_ = TypeInfo::NewWithoutFactory<SystemSubQueue>(
      "SystemSubQueue", "a queue for secondary tasks executed by main thread");

  SystemSubQueue() : File(type_) { }

  void Push(Task&& task, std::string_view msg) noexcept override {
    File::QueueSubTask(std::move(task), msg);
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object&) {
    return std::make_unique<SystemSubQueue>();
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_nil();
  }
  std::unique_ptr<File> Clone() const noexcept override {
    return std::make_unique<SystemSubQueue>();
  }

  Time lastModified() const noexcept override { return {}; }

  void* iface(const std::type_index& idx) noexcept override {
    if (idx == typeid(iface::Queue)) return static_cast<iface::Queue*>(this);
    return nullptr;
  }
};

class SystemCpuQueue : public File, private iface::SimpleQueue {
 public:
  static inline TypeInfo* type_ = TypeInfo::NewWithoutFactory<SystemCpuQueue>(
      "SystemCpuQueue", "a queue for tasks executed by sub thread");

  SystemCpuQueue() : SystemCpuQueue(3) { }
  SystemCpuQueue(size_t n) : File(type_), alive_(true) {
    assert(n > 0);
    th_.resize(n);
    for (auto& t : th_) t = std::thread(std::bind(&SystemCpuQueue::Main, this));
  }
  ~SystemCpuQueue() {
    alive_ = false;
    cv_.notify_all();
    for (auto& t : th_) t.join();
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object&) {
    return std::make_unique<SystemCpuQueue>();
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_nil();
  }
  std::unique_ptr<File> Clone() const noexcept override {
    return std::make_unique<SystemCpuQueue>();
  }

  Time lastModified() const noexcept override { return {}; }

  void* iface(const std::type_index& idx) noexcept override {
    if (idx == typeid(iface::Queue)) return static_cast<iface::Queue*>(this);
    return nullptr;
  }

 private:
  void Main() noexcept {
    while (alive_) {
      Wait();
      Item item;
      while (Pop(item)) {
        try {
          item.task();
        } catch (Exception& e) {
          assert(false);  // TODO(falsycat): error handling
        }
      }
    }
  }

  std::atomic<bool> alive_;

  std::vector<std::thread> th_;
};

class SystemImGuiConfig : public File {
 public:
  static inline TypeInfo* type_ = TypeInfo::NewWithoutFactory<SystemImGuiConfig>(
      "SystemImGuiConfig", "save/restore ImGui config");

  SystemImGuiConfig() : File(type_) { }

  std::unique_ptr<File> Clone() const noexcept override {
    return std::make_unique<SystemImGuiConfig>();
  }

  static std::unique_ptr<File> Deserialize(const msgpack::object& obj) {
    if (obj.type == msgpack::type::STR) {
      const auto& str = obj.via.str;
      ImGui::LoadIniSettingsFromMemory(str.ptr, str.size);
    }
    return std::make_unique<SystemImGuiConfig>();
  }
  void Serialize(Packer& pk) const noexcept override {
    size_t n;
    const char* ini = ImGui::SaveIniSettingsToMemory(&n);
    pk.pack_str(static_cast<uint32_t>(n));
    pk.pack_str_body(ini, static_cast<uint32_t>(n));
  }

  Time lastModified() const noexcept override { return {}; }
};

}  // namespace kingtaker
