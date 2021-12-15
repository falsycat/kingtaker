#include "kingtaker.hh"

#include <cassert>
#include <sstream>

#include "iface/queue.hh"

namespace kingtaker {

std::string Exception::Stringify() noexcept {
  std::stringstream st;
  st << msg_ << std::endl;
  st << "==== STACKTRACE ====" << std::endl;
  st << strace_ << std::endl;
  return st.str();
}


File::Path File::ParsePath(std::string_view path) noexcept {
  Path ret;
  while (path.size()) {
    const auto a = path.find_first_not_of('/');
    if (a != std::string::npos) path.remove_prefix(a);

    const auto name = path.substr(0, path.find('/'));
    path.remove_prefix(name.size());

    if (name.size()) ret.emplace_back(name);
  }
  return ret;
}
std::string File::StringifyPath(const Path& p) noexcept {
  std::string ret = "/";
  for (const auto& name : p) {
    if (ret.back() != '/') ret.push_back('/');
    ret += name;
  }
  return ret;
}

const File::TypeInfo* File::Lookup(const std::string& name) noexcept {
  auto itr = registry_.find(name);
  if (itr == registry_.end()) return nullptr;
  return itr->second;
}
std::unique_ptr<File> File::Deserialize(const msgpack::object& v) {
  try {
    const auto name = msgpack::find(v, "type"s).as<std::string>();
    const auto type = Lookup(name);
    if (!type) throw DeserializeException(std::string("unknown file type: ")+name);
    return type->Deserialize(msgpack::find(v, "param"s));
  } catch (msgpack::type_error& e) {
    throw DeserializeException("invalid File data");
  }
}
std::unique_ptr<File> File::Deserialize(std::istream& st) {
  const std::string buf(std::istreambuf_iterator<char>(st), {});
  msgpack::object_handle obj;
  msgpack::unpack(obj, buf.data(), buf.size());
  return Deserialize(obj.get());
}

void File::QueueMainTask(std::function<void()>&& task, std::string_view msg) noexcept {
  extern iface::SimpleQueue mainq_;
  mainq_.Push(std::move(task), msg);
}
void File::QueueSubTask(std::function<void()>&& task, std::string_view msg) noexcept {
  extern iface::SimpleQueue subq_;
  subq_.Push(std::move(task), msg);
}

void File::SerializeWithTypeInfo(Packer& pk) const noexcept {
  pk.pack_map(2);
  pk.pack("type");
  pk.pack(type().name());
  pk.pack("param");
  Serialize(pk);
}


File::TypeInfo::TypeInfo(const char* name, const char* desc,
                         Deserializer&& des, Factory&& fac) noexcept :
    name_(name), desc_(desc),
    deserializer_(std::move(des)), factory_(std::move(fac)) {
  assert(registry_.find(name) == registry_.end());
  registry_[name] = this;
}


void File::RefStack::Push(Term&& term) noexcept {
  terms_.push_back(std::move(term));
}
void File::RefStack::Pop() noexcept {
  terms_.pop_back();
}

std::string File::RefStack::Stringify() const noexcept {
  std::string ret = "/";
  for (const auto& term : terms_) {
    if (ret.back() != '/') ret.push_back('/');
    ret += term.name();
  }
  return ret;
}

File::RefStack File::RefStack::Resolve(const Path& p) const {
  auto a = *this;
  if (!a.ResolveInplace(p)) {
    throw NotFoundException("Resolving failed: "+StringifyPath(p));
  }
  return a;
}

File::RefStack File::RefStack::ResolveUpward(const Path& p) const {
  auto a = *this;
  for (;;) {
    auto b = a;
    if (b.ResolveInplace(p)) return b;
    if (a.terms_.empty()) {
      throw NotFoundException("Upward resolving failed: "+StringifyPath(p));
    }
    a.Pop();
  }
  
}

bool File::RefStack::ResolveInplace(const Path& p) {
  for (const auto& name : p) {
    if (name == "..") {
      Pop();
    } else if (name == ":") {
      terms_.clear();
    } else {
      auto f = (**this).Find(name);
      if (!f) return false;
      Push(Term(name, f));
    }
  }
  return true;
}

}  // namespace kingtaker
