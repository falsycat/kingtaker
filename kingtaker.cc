#include "kingtaker.hh"

#include <cassert>
#include <sstream>

namespace kingtaker {

// To ensure that the registry is initialized before using,
// define it as a static variable of function.
static File::Registry& registry_() noexcept {
  static File::Registry reg;
  return reg;
}


std::string Exception::Stringify() const noexcept {
  std::stringstream st;
  st << msg_ << "\n";
  st << "IN   " << loc_.function_name() << "\n";
  st << "FROM " << loc_.file_name() << ":" << loc_.line() << ":" << loc_.column() << std::endl;
  return st.str();
}
std::string HeavyException::Stringify() const noexcept {
  std::stringstream st;
  st << Exception::Stringify();
  st << "==== STACKTRACE ====\n";
  st << strace_ << std::endl;
  return st.str();
}


File::Path File::ParsePath(std::string_view path) noexcept {
  Path ret;
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
  std::string ret;
  for (const auto& name : p) {
    ret.push_back('/');
    ret += name;
  }
  return ret;
}

const File::Registry& File::registry() noexcept { return registry_(); }
const File::TypeInfo* File::Lookup(const std::string& name) noexcept {
  auto& reg = registry_();
  auto itr = reg.find(name);
  if (itr == reg.end()) return nullptr;
  return itr->second;
}
std::unique_ptr<File> File::Deserialize(Env* env, const msgpack::object& v) {
  std::string tname;
  try {
    tname = msgpack::find(v, "type"s).as<std::string>();
    const auto type = Lookup(tname);

    if (!type) throw DeserializeException(std::string("unknown file type: ")+tname);
    return type->Deserialize(env, msgpack::find(v, "param"s));

  } catch (msgpack::type_error&) {
    throw DeserializeException(tname.empty()? "broken File"s: "broken "+tname);
  }
}
std::unique_ptr<File> File::Deserialize(Env* env, std::istream& st) {
  const std::string buf(std::istreambuf_iterator<char>(st), {});
  msgpack::object_handle obj;
  msgpack::unpack(obj, buf.data(), buf.size());
  return Deserialize(env, obj.get());
}
void File::SerializeWithTypeInfo(Packer& pk) const noexcept {
  pk.pack_map(2);
  pk.pack("type");
  pk.pack(type().name());
  pk.pack("param");
  Serialize(pk);
}

File& File::Find(std::string_view) const {
  throw NotFoundException("no children");
}


File::TypeInfo::TypeInfo(std::string_view name,
                         std::string_view desc,
                         std::vector<std::type_index>&& iface,
                         Factory&&      factory,
                         Deserializer&& deserializer) noexcept :
    name_(name), desc_(desc), iface_(std::move(iface)),
    factory_(std::move(factory)),
    deserializer_(std::move(deserializer)) {
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
  a.ResolveInplace(p);
  return a;
}

void File::RefStack::ResolveInplace(const Path& p) {
  for (const auto& name : p) {
    if (name == "..") {
      if (terms_.empty()) {
        throw NotFoundException("root has no parent");
      }
      Pop();
    } else if (name == ":") {
      terms_.clear();
    } else {
      Push({name, &(**this).Find(name)});
    }
  }
}

}  // namespace kingtaker
