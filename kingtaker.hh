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
#include <vector>

#include <boost/stacktrace.hpp>

#include <msgpack.hh>


namespace kingtaker {

using namespace std::literals;


class Exception {
 public:
  static const std::string& msg() { return msg_; }
  static const boost::stacktrace::stacktrace& stacktrace() { return strace_; }

  static std::string Stringify() noexcept;

  Exception(std::string_view msg) noexcept { msg_ = msg; strace_ = {}; }
  Exception() = delete;
  virtual ~Exception() = default;
  Exception(const Exception&) = delete;
  Exception(Exception&&) = default;
  Exception& operator=(const Exception&) = delete;
  Exception& operator=(Exception&&) = default;

 private:
  static inline std::string msg_;

  static inline boost::stacktrace::stacktrace strace_;
};


class File {
 public:
  using Clock  = std::chrono::system_clock;
  using Time   = Clock::time_point;
  using Path   = std::vector<std::string>;
  using Packer = msgpack::packer<std::ostream>;

  class TypeInfo;
  class RefStack;

  class DeserializeException;
  class NotFoundException;
  class UnsupportedException;

  static Path ParsePath(std::string_view) noexcept;
  static std::string StringifyPath(const Path&) noexcept;

  static const TypeInfo* Lookup(const std::string&) noexcept;
  static std::unique_ptr<File> Deserialize(const msgpack::object&);
  static std::unique_ptr<File> Deserialize(std::istream&);

  static void QueueMainTask(std::function<void()>&&, std::string_view = "") noexcept;
  static void QueueSubTask(std::function<void()>&&, std::string_view = "") noexcept;

  static File& root() noexcept { return *root_; }
  static void root(File* f) noexcept { assert(!root_); root_ = f; }

  static const auto& registry() noexcept { return registry_(); }

  template <typename T>
  static T* iface(File* f, T* def = nullptr) noexcept { return f->iface<T>(def); }
  template <typename T>
  static T& iface(File* f, T& def) noexcept { return *f->iface<T>(&def); }
  template <typename T>
  static T* iface(File& f, T* def = nullptr) noexcept { return f.iface<T>(def); }
  template <typename T>
  static T& iface(File& f, T& def) noexcept { return *f.iface<T>(&def); }

  File(const TypeInfo* type) noexcept : type_(type) { }
  File() = delete;
  virtual ~File() = default;
  File(const File&) = delete;
  File(File&&) = delete;
  File& operator=(const File&) = delete;
  File& operator=(File&&) = delete;

  virtual File* Find(std::string_view) const noexcept { return nullptr; }
  virtual void Scan(std::function<void(std::string_view, File*)>) const noexcept { }

  virtual void Serialize(Packer&) const noexcept = 0;
  virtual std::unique_ptr<File> Clone() const noexcept = 0;

  virtual Time lastModified() const noexcept = 0;
  virtual void* iface(const std::type_index&) noexcept { return nullptr; }

  void SerializeWithTypeInfo(Packer&) const noexcept;

  template <typename T>
  T* iface(T* def = nullptr) noexcept {
    T* ret = reinterpret_cast<T*>(iface(std::type_index(typeid(T))));
    return ret? ret: def;
  }
  const TypeInfo& type() const noexcept { return *type_; }

 private:
  static std::map<std::string, TypeInfo*>& registry_() noexcept;

  static inline File* root_ = nullptr;

  const TypeInfo* type_;
};

class File::TypeInfo final {
 public:
  using Factory      = std::function<std::unique_ptr<File>()>;
  using AssocFactory = std::function<std::unique_ptr<File>(const std::filesystem::path&)>;
  using AssocChecker = std::function<bool(const std::filesystem::path&)>;
  using Deserializer = std::function<std::unique_ptr<File>(const msgpack::object&)>;
  using GUI          = std::function<void()>;

  template <typename T>
  static TypeInfo New(std::string_view name,
                      std::string_view desc,
                      std::vector<std::type_index>&& iface) noexcept {
    Factory f;
    if constexpr (std::is_default_constructible<T>::value) {
      f = []() { return std::make_unique<T>(); };
    }
    AssocFactory af;
    if constexpr (std::is_constructible<T, const std::filesystem::path&>::value) {
      af = [](auto& p) { return std::make_unique<T>(p); };
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

  std::unique_ptr<File> Create() const noexcept {
    return factory_();
  }
  std::unique_ptr<File> CreateFromFile(const std::filesystem::path& p) const noexcept {
    return assoc_factory_(p);
  }
  bool CheckAssoc(const std::filesystem::path& p) const noexcept {
    return assoc_checker_? assoc_checker_(p): false;
  }
  std::unique_ptr<File> Deserialize(const msgpack::object& v) const {
    return deserializer_(v);
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

 private:
  template <typename T>
  static auto GetAssocChecker(int) noexcept -> decltype(T::CheckAssoc, AssocChecker()) {
    return [](auto& p) { return T::CheckAssoc(p); };
  }
  template <typename T>
  static auto GetAssocChecker(...) noexcept -> AssocChecker { return {}; }

  template <typename T>
  static auto GetDeserializer(int) noexcept -> decltype(T::Deserialize, Deserializer()) {
    return [](auto& v) { return T::Deserialize(v); };
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

  RefStack ResolveUpward(const Path& p) const;
  RefStack ResolveUpward(std::string_view p) const { return ResolveUpward(ParsePath(p)); }

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
  const Term& terms(std::size_t i) noexcept { return terms_[i]; }
  std::size_t size() const noexcept { return terms_.size(); }

 private:
  bool ResolveInplace(const Path& p);

  std::vector<Term> terms_;
};

class File::DeserializeException : public Exception {
 public:
  DeserializeException(std::string_view what) noexcept : Exception(what) { }
};
class File::NotFoundException : public Exception {
 public:
  NotFoundException(std::string_view what) noexcept : Exception(what) { }
};
class File::UnsupportedException : public Exception {
 public:
  UnsupportedException(std::string_view what) noexcept : Exception(what) { }
  UnsupportedException(const RefStack& p, std::string_view name) noexcept :
      Exception("'"+p.Stringify()+"' doesn't have '"+std::string(name)+"' interface") { }
};

}  // namespace kingtaker
