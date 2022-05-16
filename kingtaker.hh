#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <map>
#include <variant>
#include <vector>

#include <boost/stacktrace.hpp>

#include <msgpack.hh>
#include <source_location.hh>


namespace kingtaker {

using namespace std::literals;

using Clock  = std::chrono::system_clock;
using Time   = Clock::time_point;
using Packer = msgpack::packer<std::ostream>;


// All exceptions thrown by kingtaker must inherit this class.
class Exception {
 public:
  using Loc = std::source_location;

  Exception(std::string_view msg, Loc loc = Loc::current()) noexcept :
      msg_(msg), loc_(loc) {
  }

  Exception() = delete;
  virtual ~Exception() = default;
  Exception(const Exception&) = delete;
  Exception(Exception&&) = delete;
  Exception& operator=(const Exception&) = delete;
  Exception& operator=(Exception&&) = delete;

  virtual std::string Stringify() const noexcept;

  const std::string& msg() const noexcept { return msg_; }
  const Loc& loc() const noexcept { return loc_; }

 private:
  std::string msg_;

  Loc loc_;
};

// Exception with stacktrace
class HeavyException : public Exception {
 public:
  using Exception::Exception;
  std::string Stringify() const noexcept override;
 private:
  boost::stacktrace::stacktrace strace_;
};

class DeserializeException : public HeavyException {
 public:
  using HeavyException::HeavyException;
};


// Task queue. Any operations are thread-safe.
class Queue {
 public:
  using Task = std::function<void()>;

  // synchronized with kingtaker filesystem
  // and all tasks are processed on each GUI update
  static Queue& main() noexcept;

  // synchronized with kingtaker filesystem
  // some tasks might not be done if display update is done faster than them
  static Queue& sub() noexcept;

  // tasks are done in thread independent completely from kingtaker filesystem
  static Queue& cpu() noexcept;

  // synchronized with GUI update but not with filesystem
  // all tasks are processed with valid GL context on each GUI update
  static Queue& gl() noexcept;

  Queue() = default;
  virtual ~Queue() = default;
  Queue(const Queue&) = delete;
  Queue(Queue&&) = delete;
  Queue& operator=(const Queue&) = delete;
  Queue& operator=(Queue&&) = delete;

  virtual void Push(Task&&) noexcept = 0;
};


class File {
 public:
  class TypeInfo;
  class Path;
  class Env;
  class Event;

  class NotImplementedException;
  class NotFoundException;

  using Registry = std::map<std::string, TypeInfo*>;

  static const TypeInfo* Lookup(const std::string&) noexcept;
  static std::unique_ptr<File> Deserialize(Env*, const msgpack::object&);
  static std::unique_ptr<File> Deserialize(Env*, std::istream&);

  static File& root() noexcept;
  static const Registry& registry() noexcept;

  File(const TypeInfo* type, Env* env, Time lastmod = Clock::now()) noexcept :
      type_(type), env_(env), lastmod_(lastmod) {
  }
  File() = delete;
  virtual ~File() = default;
  File(const File&) = delete;
  File(File&&) = delete;
  File& operator=(const File&) = delete;
  File& operator=(File&&) = delete;

  virtual void Serialize(Packer&) const noexcept = 0;
  virtual std::unique_ptr<File> Clone(Env*) const noexcept = 0;

  // Calls Serialize() after packing TypeInfo.
  // To make it available to deserialize by File::Deserialize(),
  // use this instead of Serialize().
  void SerializeWithTypeInfo(Packer&) const noexcept;

  // Be called on each GUI updates.
  virtual void Update(Event&) noexcept { }

  // To make children referrable by path specification, returns them.
  // If there's no such child, throw NotFoundException.
  virtual File& Find(std::string_view) const;

  // Returns a file specified by the relative path or throws NotFoundException.
  File& Resolve(const Path&) const;
  File& Resolve(std::string_view) const;
  File& ResolveUpward(const Path&) const;
  File& ResolveUpward(std::string_view) const;

  // Sets lastmod to current time.
  void Touch() noexcept;

  // Notifies this file is moved under new parent with new name.
  void Move(File*, std::string_view) noexcept;

  Path abspath() const noexcept;

  // Takes typeinfo of the requested interface and
  // returns a pointer of the implementation or nullptr if not implemented.
  virtual void* iface(const std::type_index&) noexcept { return nullptr; }
  template <typename T>
  T* iface() noexcept {
    return reinterpret_cast<T*>(iface(typeid(T)));
  }
  template <typename T>
  T& ifaceOrThrow();

  const TypeInfo& type() const noexcept { return *type_; }
  Env& env() const noexcept { return *env_; }
  Time lastmod() const noexcept { return lastmod_; }
  File* parent() const noexcept { return parent_; }
  const std::string& name() const noexcept { return name_; }

 private:
  const TypeInfo* type_;

  Env* env_;

  Time lastmod_;

  File* parent_ = nullptr;

  std::string name_;
};

class File::TypeInfo final {
 public:
  using Factory      = std::function<std::unique_ptr<File>(Env*)>;
  using Deserializer = std::function<std::unique_ptr<File>(Env*, const msgpack::object&)>;

  template <typename T>
  static TypeInfo New(std::string_view name,
                      std::string_view desc,
                      std::vector<std::type_index>&& iface) noexcept {
    static_assert(std::is_constructible<T, Env*, const msgpack::object&>::value,
                  "T has no deserializer");

    Factory factory;
    if constexpr (std::is_constructible<T, Env*>::value) {
      factory = [](auto* env) { return std::make_unique<T>(env); };
    }

    Deserializer deserializer =
        [](auto* env, auto& obj) { return std::make_unique<T>(env, obj); };

    return TypeInfo(name, desc, std::move(iface),
                    std::move(factory),
                    std::move(deserializer));
  }

  TypeInfo(std::string_view,
           std::string_view,
           std::vector<std::type_index>&&,
           Factory&&,
           Deserializer&&) noexcept;
  ~TypeInfo() noexcept;
  TypeInfo() = delete;
  TypeInfo(const TypeInfo&) = delete;
  TypeInfo(TypeInfo&&) = default;
  TypeInfo& operator=(const TypeInfo&) = delete;
  TypeInfo& operator=(TypeInfo&&) = delete;

  std::unique_ptr<File> Create(Env* env) const {
    return factory_(env);
  }
  std::unique_ptr<File> Deserialize(Env* env, const msgpack::object& v) const {
    return deserializer_(env, v);
  }

  template <typename T>
  bool IsImplemented() const noexcept {
    return iface_.end() != std::find(iface_.begin(), iface_.end(), typeid(T));
  }

  const std::string& name() const noexcept { return name_; }
  const std::string& desc() const noexcept { return desc_; }

  bool factory() const noexcept { return !!factory_; }
  bool deserializer() const noexcept { return !!deserializer_; }

 private:
  std::string name_;

  std::string desc_;

  std::vector<std::type_index> iface_;

  Factory factory_;

  Deserializer deserializer_;
};

class File::Path final {
 public:
  Path() = default;
  Path(std::initializer_list<std::string> terms) noexcept :
      terms_(terms.begin(), terms.end()) {
  }
  Path(std::vector<std::string>&& terms) noexcept :
      terms_(std::move(terms)) {
  }
  Path(const Path&) = default;
  Path(Path&&) = default;
  Path& operator=(const Path&) = default;
  Path& operator=(Path&&) = default;

  bool operator==(const Path& other) const noexcept {
    return terms_ == other.terms_;
  }
  bool operator!=(const Path& other) const noexcept {
    return terms_ != other.terms_;
  }

  static Path Parse(std::string_view) noexcept;
  std::string Stringify() const noexcept;

  std::span<const std::string> terms() const noexcept { return terms_; }
  std::vector<std::string>& terms() noexcept { return terms_; }

 private:
  std::vector<std::string> terms_;
};

class File::Env final {
 public:
  enum Flag : uint8_t {
    kNone     = 0,
    kRoot     = 1 << 1,
    kVolatile = 1 << 2,
  };
  using Flags = uint8_t;

  Env() = delete;
  Env(const std::filesystem::path& npath, Flags flags) noexcept :
      npath_(npath), flags_(flags) {
  }
  Env(const Env&) = delete;
  Env(Env&&) = delete;
  Env& operator=(const Env&) = delete;
  Env& operator=(Env&&) = delete;

  const std::filesystem::path& npath() const noexcept { return npath_; }
  Flags flags() const noexcept { return flags_; }

 private:
  std::filesystem::path npath_;

  Flags flags_;
};

class File::Event {
 public:
  enum State : uint8_t {
    kNone    = 0,
    kClosing = 1 << 0,
    kClosed  = 1 << 1,
    kSaved   = 1 << 2,
  };
  using Status = uint8_t;

  Event() = delete;
  Event(const Event&) = delete;
  Event(Event&&) = delete;
  Event& operator=(const Event&) = delete;
  Event& operator=(Event&&) = delete;

  virtual void CancelClosing(File*, std::string_view = "") noexcept = 0;
  virtual void Focus(File*) noexcept = 0;

  bool IsFocused(File* f) const noexcept { return focus_.contains(f); }

  bool closing() const noexcept { return status_ & kClosing; }
  bool closed()  const noexcept { return status_ & kClosed;  }
  bool saved()   const noexcept { return status_ & kSaved;   }

 protected:
  Event(Status st, std::unordered_set<File*>&& f) noexcept :
      status_(st), focus_(std::move(f)) {
  }

 private:
  Status status_;

  std::unordered_set<File*> focus_;
};

class File::NotImplementedException : public Exception {
 public:
  using Exception::Exception;
};
template <typename T>
T& File::ifaceOrThrow() {
  if (auto ret = iface<T>()) return *ret;
  throw NotImplementedException(
      typeid(T).name()+" is not implemented: "s+abspath().Stringify());
}

class File::NotFoundException : public Exception {
 public:
  using Exception::Exception;
};

}  // namespace kingtaker
