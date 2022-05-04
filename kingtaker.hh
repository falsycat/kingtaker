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
// Saves stacktrace but a bit heavy so don't use many times.
class HeavyException : public Exception {
 public:
  HeavyException(std::string_view msg, Loc loc = Loc::current()) noexcept :
      Exception(msg, loc) {
  }
  std::string Stringify() const noexcept override;
 private:
  boost::stacktrace::stacktrace strace_;
};
class DeserializeException : public HeavyException {
 public:
  DeserializeException(std::string_view msg, Loc loc = Loc::current()) noexcept :
      HeavyException(msg, loc) {
  }
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
  class RefStack;
  class Path;
  class Env;
  class Event;

  class NotFoundException;

  using Registry = std::map<std::string, TypeInfo*>;

  static const TypeInfo* Lookup(const std::string&) noexcept;
  static std::unique_ptr<File> Deserialize(Env*, const msgpack::object&);
  static std::unique_ptr<File> Deserialize(Env*, std::istream&);

  template <typename T>
  static T* iface(File* f, T* def = nullptr) noexcept {
    auto ret = reinterpret_cast<T*>(f->iface(typeid(T)));
    return ret? ret: def;
  }

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
  virtual void Update(RefStack&, Event&) noexcept { }

  // To make children referrable by path specification, returns them.
  virtual File& Find(std::string_view) const;

  // Sets lastmod to current time.
  void Touch() noexcept { lastmod_ = Clock::now(); }

  // Notifies this file is moved under new parent.
  void Move(File* parent) noexcept { parent_ = parent; }

  // Takes typeinfo of the requested interface and
  // returns a pointer of the implementation or nullptr if not implemented.
  virtual void* iface(const std::type_index&) noexcept { return nullptr; }

  const TypeInfo& type() const noexcept { return *type_; }
  Env& env() const noexcept { return *env_; }
  Time lastmod() const noexcept { return lastmod_; }
  File* parent() const noexcept { return parent_; }

 private:
  const TypeInfo* type_;

  Env* env_;

  Time lastmod_;

  File* parent_ = nullptr;
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

class File::Path final : public std::vector<std::string> {
 public:
  using vector::vector;

  static Path Parse(std::string_view) noexcept;
  std::string Stringify() const noexcept;
};

class File::RefStack final {
 public:
  struct Term {
   public:
    Term(std::string_view name, File* file) noexcept : name_(name), file_(file) { }
    Term() = default;
    Term(const Term&) = default;
    Term(Term&&) = default;
    Term& operator=(const Term&) = default;
    Term& operator=(Term&&) = default;

    const std::string& name() const noexcept { return name_; }
    File& file() const noexcept { return *file_; }

   private:
    std::string name_;

    File* file_;
  };

  RefStack() = default;
  RefStack(const RefStack&) = default;
  RefStack(RefStack&&) = default;
  RefStack& operator=(const RefStack&) = default;
  RefStack& operator=(RefStack&&) = default;

  File& operator*() const noexcept { return terms_.empty()? root(): terms_.back().file(); }

  void Push(Term&&) noexcept;
  void Pop() noexcept;

  RefStack Resolve(const Path& p) const;
  RefStack Resolve(std::string_view p) const { return Resolve(Path::Parse(p)); }

  Path GetFullPath() const noexcept;
  std::string Stringify() const noexcept;

  const Term& top() const noexcept { return terms_.back(); }
  const Term& terms(std::size_t i) const noexcept { return terms_[i]; }
  std::size_t size() const noexcept { return terms_.size(); }

 private:
  void ResolveInplace(const Path& p);

  std::vector<Term> terms_;
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

class File::NotFoundException : public Exception {
 public:
  NotFoundException(std::string_view msg, Loc loc = Loc::current()) noexcept :
      Exception(msg, loc) {
  }
};

}  // namespace kingtaker
