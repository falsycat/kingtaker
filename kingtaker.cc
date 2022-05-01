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

File::File(const TypeInfo* type, Env* env, Time lastmod) noexcept :
    type_(type), env_(env), lastmod_(lastmod) {
}
File::~File() noexcept {
  for (auto& obs : obs_) obs->ObserveDie();
  assert(obs_.size() == 0);
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

void File::Touch() noexcept {
  lastmod_ = Clock::now();
  for (auto& obs : obs_) obs->ObserveChange();
}
void File::MoveUnder(File* parent) noexcept {
  parent_ = parent;
  for (auto& obs : obs_) obs->ObserveMove();
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


File::Observer::Observer(File* target) noexcept : target_(target) {
  target->obs_.push_back(this);
}
File::Observer::~Observer() noexcept {
  if (target_) ObserveDie();
}
void File::Observer::ObserveDie() noexcept {
  auto& obs = target_->obs_;
  obs.erase(std::remove(obs.begin(), obs.end(), this), obs.end());
  target_ = nullptr;
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
  if (!a.ResolveInplace(p)) throw NotFoundException(p, a);
  return a;
}

bool File::RefStack::ResolveInplace(const Path& p) {
  for (const auto& name : p) {
    if (name == "..") {
      if (terms_.empty()) return false;
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
