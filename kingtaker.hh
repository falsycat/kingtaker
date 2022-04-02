#pragma once

#include <algorithm>
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
  class Env;
  class RefStack;
  class Event;

  class NotFoundException;

  using Clock    = std::chrono::system_clock;
  using Time     = Clock::time_point;
  using Path     = std::vector<std::string>;
  using Packer   = msgpack::packer<std::ostream>;
  using Registry = std::map<std::string, TypeInfo*>;

  static Path ParsePath(std::string_view) noexcept;
  static std::string StringifyPath(const Path&) noexcept;

  static const TypeInfo* Lookup(const std::string&) noexcept;
  static std::unique_ptr<File> Deserialize(const msgpack::object&, const std::shared_ptr<Env>&);
  static std::unique_ptr<File> Deserialize(std::istream&, const std::shared_ptr<Env>&);

  // An entrypoint must set root file by calling root(File*) before entering main loop.
  static File& root() noexcept;

  static const Registry& registry() noexcept { return registry_(); }

  // Use these static version of iface() when compiler cannot
  // find non-static template member.
  template <typename T>
  static T* iface(File* f, T* def = nullptr) noexcept { return f->iface<T>(def); }
  template <typename T>
  static T& iface(File* f, T& def) noexcept { return f->iface<T>(def); }
  template <typename T>
  static T* iface(File& f, T* def = nullptr) noexcept { return f.iface<T>(def); }
  template <typename T>
  static T& iface(File& f, T& def) noexcept { return f.iface<T>(def); }

  File(const TypeInfo* type, const std::shared_ptr<Env>& env) noexcept :
      type_(type), env_(env) {
  }
  File() = delete;
  virtual ~File() = default;
  File(const File&) = delete;
  File(File&&) = delete;
  File& operator=(const File&) = delete;
  File& operator=(File&&) = delete;

  virtual void Serialize(Packer&) const noexcept = 0;
  virtual std::unique_ptr<File> Clone(const std::shared_ptr<Env>&) const noexcept = 0;

  // To make children referrable by path specification, returns them.
  virtual File* Find(std::string_view) const noexcept { return nullptr; }

  // Be called on each GUI updates.
  virtual void Update(RefStack&, Event&) noexcept { }

  // Some features may use this field to detect changes.
  virtual Time lastmod() const noexcept { return {}; }

  // Takes typeinfo of the requested interface and
  // returns a pointer of the implementation or nullptr if not implemented.
  virtual void* iface(const std::type_index&) noexcept { return nullptr; }

  // Calls Serialize() after packing TypeInfo.
  // To make it available to deserialize by File::Deserialize(),
  // use this instead of Serialize().
  void SerializeWithTypeInfo(Packer&) const noexcept;

  const TypeInfo& type() const noexcept { return *type_; }
  const std::shared_ptr<Env>& env() const noexcept { return env_; }

 private:
  static Registry& registry_() noexcept { static Registry reg_; return reg_; }

  const TypeInfo* type_;

  std::shared_ptr<Env> env_;


  template <typename T>
  T* iface(T* def = nullptr) noexcept {
    T* ret = reinterpret_cast<T*>(iface(std::type_index(typeid(T))));
    return ret? ret: def;
  }
  template <typename T>
  T& iface(T& def) noexcept { return *iface<T>(&def); }
};

class File::TypeInfo final {
 public:
  using Factory      = std::function<std::unique_ptr<File>(const std::shared_ptr<Env>&)>;
  using AssocFactory = std::function<std::unique_ptr<File>(const std::filesystem::path&, const std::shared_ptr<Env>&)>;
  using AssocChecker = std::function<bool(const std::filesystem::path&)>;
  using Deserializer = std::function<std::unique_ptr<File>(const msgpack::object&, const std::shared_ptr<Env>&)>;
  using GUI          = std::function<void()>;

  template <typename T>
  static TypeInfo New(std::string_view name,
                      std::string_view desc,
                      std::vector<std::type_index>&& iface) noexcept {
    Factory f;
    if constexpr (std::is_constructible<T, std::shared_ptr<Env>>::value) {
      f = [](auto& env) { return std::make_unique<T>(env); };
    }
    AssocFactory af;
    if constexpr (std::is_constructible<T, const std::filesystem::path&>::value) {
      af = [](auto& p, auto& env) { return std::make_unique<T>(p, env); };
    }
    return TypeInfo(name, desc, std::move(iface),
                    std::move(f),
                    std::move(af),
                    GetAssocChecker<T>(0),
                    GetDeserializer<T>(0),
                    GetGUI<T>(0));
  }

  TypeInfo(std::string_view,
           std::string_view,
           std::vector<std::type_index>&&,
           Factory&&,
           AssocFactory&&,
           AssocChecker&&,
           Deserializer&&,
           GUI&&) noexcept;
  ~TypeInfo() noexcept;
  TypeInfo() = delete;
  TypeInfo(const TypeInfo&) = delete;
  TypeInfo(TypeInfo&&) = default;
  TypeInfo& operator=(const TypeInfo&) = delete;
  TypeInfo& operator=(TypeInfo&&) = delete;

  std::unique_ptr<File> Create(const std::shared_ptr<Env>& env) const noexcept {
    return factory_(env);
  }
  std::unique_ptr<File> CreateFromFile(const std::filesystem::path& p, const std::shared_ptr<Env>& env) const noexcept {
    return assoc_factory_(p, env);
  }
  bool CheckAssoc(const std::filesystem::path& p) const noexcept {
    return assoc_checker_? assoc_checker_(p): false;
  }
  std::unique_ptr<File> Deserialize(const msgpack::object& v, const std::shared_ptr<Env>& env) const {
    return deserializer_(v, env);
  }

  void UpdateGUI() const noexcept {
    if (gui_) gui_();
  }

  template <typename T>
  bool CheckImplemented() const noexcept {
    return iface_.end() != std::find(iface_.begin(), iface_.end(), typeid(T));
  }

  const std::string& name() const noexcept { return name_; }
  const std::string& desc() const noexcept { return desc_; }

  bool factory() const noexcept { return !!factory_; }
  bool assocFactory() const noexcept { return !!assoc_factory_; }
  bool deserializer() const noexcept { return !!deserializer_; }
  bool gui() const noexcept { return !!gui_; }

 private:
  template <typename T>
  static auto GetAssocChecker(int) noexcept -> decltype(T::CheckAssoc, AssocChecker()) {
    return [](auto& p) { return T::CheckAssoc(p); };
  }
  template <typename T>
  static auto GetAssocChecker(...) noexcept -> AssocChecker { return {}; }

  template <typename T>
  static auto GetDeserializer(int) noexcept -> decltype(T::Deserialize, Deserializer()) {
    return [](auto& v, auto& env) { return T::Deserialize(v, env); };
  }
  template <typename T>
  static auto GetDeserializer(...) noexcept -> Deserializer { return {}; }

  template <typename T>
  static auto GetGUI(int) noexcept -> decltype(T::UpdateTypeInfo, GUI()) {
    return [](auto& v) { return T::UpdateTypeInfo(v); };
  }
  template <typename T>
  static auto GetGUI(...) noexcept -> GUI { return {}; }

  std::string name_;

  std::string desc_;

  std::vector<std::type_index> iface_;

  Factory factory_;

  AssocFactory assoc_factory_;

  AssocChecker assoc_checker_;

  Deserializer deserializer_;

  GUI gui_;
};

class File::Env {
 public:
  enum Flag : uint8_t {
    kNone     = 0,
    kRoot     = 1 << 1,
    kReadOnly = 1 << 2,  // permanentize is disabled
  };
  using Flags = uint8_t;

  Env() = delete;
  Env(const std::filesystem::path& path, Flags flags) noexcept :
      path_(path), flags_(flags) {
  }
  virtual ~Env() = default;
  Env(const Env&) = delete;
  Env(Env&&) = delete;
  Env& operator=(const Env&) = delete;
  Env& operator=(Env&&) = delete;

  const std::filesystem::path& path() const noexcept { return path_; }
  Flags flags() const noexcept { return flags_; }

 private:
  std::filesystem::path path_;

  Flags flags_;
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
  RefStack Resolve(std::string_view p) const { return Resolve(ParsePath(p)); }

  Path GetFullPath() const noexcept;
  std::string Stringify() const noexcept;

  template <typename T>
  T* FindParent() const noexcept {
    for (auto itr = terms_.crbegin(); itr < terms_.crend(); ++itr) {
      auto& f = itr->file();
      auto ret = dynamic_cast<T*>(&f);
      if (ret) return ret;
    }
    return nullptr;
  }

  const Term& top() const noexcept { return terms_.back(); }
  const Term& terms(std::size_t i) const noexcept { return terms_[i]; }
  std::size_t size() const noexcept { return terms_.size(); }

 private:
  bool ResolveInplace(const Path& p);

  std::vector<Term> terms_;
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
  NotFoundException(const Path& p, const RefStack& st, Loc loc = Loc::current()) noexcept :
      Exception(
          "file not found in '"s+st.Stringify()+
          "' while resolving '"s+StringifyPath(p)+"'"s, loc) {
  }
};

}  // namespace kingtaker
