#include "kingtaker.hh"

#include <cassert>
#include <sstream>

namespace kingtaker {

std::string                   Exception::msg_;
boost::stacktrace::stacktrace Exception::strace_;

std::string Exception::Stringify() noexcept {
  std::stringstream st;
  st << msg_ << std::endl;
  st << "==== STACKTRACE ====" << std::endl;
  st << strace_ << std::endl;
  return st.str();
}


File::Path File::ParsePath(std::string_view path) noexcept {
  Path ret = {":"};
  while (path.size()) {
    const auto a = path.find_first_not_of('/');
    if (a != std::string::npos) {
      path.remove_prefix(a);
    } else {
      return ret;
    }

    const auto name = path.substr(0, path.find('/'));
    path.remove_prefix(name.size());

    if (name.size()) ret.emplace_back(name);
  }
  return ret;
}
std::string File::StringifyPath(const Path& p) noexcept {
  std::string ret = ":";
  for (const auto& name : p) {
    ret.push_back('/');
    ret += name;
  }
  return ret;
}

const File::TypeInfo* File::Lookup(const std::string& name) noexcept {
  auto& reg = registry_();
  auto itr = reg.find(name);
  if (itr == reg.end()) return nullptr;
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

void File::SerializeWithTypeInfo(Packer& pk) const noexcept {
  pk.pack_map(2);
  pk.pack("type");
  pk.pack(type().name());
  pk.pack("param");
  Serialize(pk);
}


File::TypeInfo::TypeInfo(std::string_view name,
                         std::string_view desc,
                         std::vector<std::type_index>&& iface,
                         Factory&& f,
                         AssocFactory&& af,
                         AssocChecker&& ac,
                         Deserializer&& d,
                         GUI&& g) noexcept :
    name_(name), desc_(desc), iface_(std::move(iface)),
    factory_(std::move(f)),
    assoc_factory_(std::move(af)),
    assoc_checker_(std::move(ac)),
    deserializer_(std::move(d)),
    gui_(std::move(g)) {
  auto& reg = registry_();
  assert(reg.find(name_) == reg.end());
  reg[name_] = this;
}

File::TypeInfo::~TypeInfo() noexcept {
  registry_().erase(name_);
}


void File::RefStack::Push(Term&& term) noexcept {
  terms_.push_back(std::move(term));
}
void File::RefStack::Pop() noexcept {
  terms_.pop_back();
}

File::Path File::RefStack::GetFullPath() const noexcept {
  Path ret;
  ret.reserve(terms_.size());
  for (const auto& term : terms_) {
    ret.push_back(term.name());
  }
  return ret;
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
