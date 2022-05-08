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
  st << "FROM " << loc_.file_name() << "$" << loc_.line() << "$" << loc_.column() << std::endl;
  return st.str();
}
std::string HeavyException::Stringify() const noexcept {
  std::stringstream st;
  st << Exception::Stringify();
  st << "==== STACKTRACE ====\n";
  st << strace_ << std::endl;
  return st.str();
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
File& File::Resolve(const Path& p) const {
  auto ret = const_cast<File*>(this);
  for (const auto& term : p.terms()) {
    if (term == "..") {
      ret = parent_;
    } else if (term == ".") {
      // do nothing
    } else if (term == "$") {
      ret = &root();
    } else {
      ret = &ret->Find(term);
    }
  }
  return *ret;
}
File& File::Resolve(std::string_view p) const {
  return Resolve(Path::Parse(p));
}
File& File::ResolveUpward(const Path& p) const {
  auto base = const_cast<File*>(this);
  while (base) {
    try {
      return base->Resolve(p);
    } catch (NotFoundException&) {
      base = base->parent_;
    }
  }
  throw NotFoundException("ResolveUpward failed: "s+p.Stringify());
}
File& File::ResolveUpward(std::string_view p) const {
  return ResolveUpward(Path::Parse(p));
}

void File::Touch() noexcept {
  lastmod_ = Clock::now();
}
void File::Move(File* parent, std::string_view name) noexcept {
  parent_ = parent;
  name_   = name;
}

File::Path File::abspath() const noexcept {
  std::vector<std::string> terms;
  for (auto f = this; f->parent_; f = f->parent_) {
    terms.push_back(f->name_);
  }
  terms.push_back("$");
  std::reverse(terms.begin(), terms.end());
  return {std::move(terms)};
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


File::Path File::Path::Parse(std::string_view path) noexcept {
  std::vector<std::string> terms;
  while (path.size()) {
    const auto a = path.find_first_not_of('/');
    if (a != std::string::npos) {
      path.remove_prefix(a);
    } else {
      return {std::move(terms)};
    }

    const auto name = path.substr(0, path.find('/'));
    path.remove_prefix(name.size());

    if (name.size()) terms.emplace_back(name);
  }
  return {std::move(terms)};
}
std::string File::Path::Stringify() const noexcept {
  std::string ret;
  for (const auto& name : terms_) {
    ret += name;
    ret.push_back('/');
  }
  return ret;
}

}  // namespace kingtaker
