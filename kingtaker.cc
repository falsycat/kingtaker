#include "kingtaker.hh"

#include <cassert>
#include <sstream>

#include "iface/queue.hh"

namespace kingtaker {

std::string                   Exception::msg_;
boost::stacktrace::stacktrace Exception::strace_;

File* File::root_ = nullptr;


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

std::map<std::string, File::TypeInfo*>& File::registry_() noexcept {
  static std::map<std::string, TypeInfo*> registry_;
  return registry_;
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


void Value::Serialize(File::Packer& pk) const {
  if (has<Integer>()) {
    pk.pack(get<Integer>());
    return;
  }
  if (has<Scalar>()) {
    pk.pack(get<Scalar>());
    return;
  }
  if (has<Boolean>()) {
    pk.pack(get<Boolean>());
    return;
  }
  if (has<String>()) {
    pk.pack(get<String>());
    return;
  }
  if (has<Vec2>()) {
    auto& v = get<Vec2>();
    pk.pack_array(2);
    pk.pack(v.x); pk.pack(v.y);
    return;
  }
  if (has<Vec3>()) {
    auto& v = get<Vec3>();
    pk.pack_array(3);
    pk.pack(v.x); pk.pack(v.y); pk.pack(v.z);
    return;
  }
  if (has<Vec4>()) {
    auto& v = get<Vec4>();
    pk.pack_array(4);
    pk.pack(v.x); pk.pack(v.y); pk.pack(v.z); pk.pack(v.w);
    return;
  }
  if (has<Tensor>()) {
    auto& v = get<Tensor>();
    pk.pack_map(2);
    pk.pack("tensor"s);
    v.Serialize(pk);
    return;
  }
  throw Exception("serialization is not supported on the type");
}
Value Value::Deserialize(const msgpack::object& obj) {
  try {
    switch (obj.type) {
    case msgpack::type::BOOLEAN:
      return Value(obj.via.boolean);
    case msgpack::type::POSITIVE_INTEGER:
    case msgpack::type::NEGATIVE_INTEGER:
      return Value(static_cast<Integer>(obj.via.i64));
    case msgpack::type::FLOAT:
      return Value(obj.via.f64);
    case msgpack::type::STR:
      return Value(
          std::string(obj.via.str.ptr, obj.via.str.size));
    case msgpack::type::ARRAY:
      switch (obj.via.array.size) {
      case 2: {
        const auto v = obj.as<std::array<double, 2>>();
        return Vec2(v[0], v[1]);
      }
      case 3: {
        const auto v = obj.as<std::array<double, 3>>();
        return Vec3(v[0], v[1], v[2]);
      }
      case 4: {
        const auto v = obj.as<std::array<double, 4>>();
        return Vec4(v[0], v[1], v[2], v[3]);
      } }
      break;
    case msgpack::type::MAP: {
      const auto  type  = msgpack::find(obj, "type"s).as<std::string>();
      const auto& param = msgpack::find(obj, "param"s);
      if (type == "tensor") return Tensor::Deserialize(param);
    } break;

    default:
      ;
    }
  } catch (msgpack::type_error& e) {
  }
  throw DeserializeException("invalid value");
}


std::string_view Value::Tensor::StringifyType(Type t) noexcept {
  switch (t) {
  case I8:  return "i8";
  case I16: return "i16";
  case I32: return "i32";
  case I64: return "i64";
  case U8:  return "u8";
  case U16: return "u16";
  case U32: return "u32";
  case U64: return "u64";
  case F16: return "f16";
  case F32: return "f32";
  case F64: return "f64";
  }
  return "";
}
Value::Tensor::Type Value::Tensor::ParseType(std::string_view v) {
  if (v == "i8" ) return I8;
  if (v == "i16") return I16;
  if (v == "i32") return I32;
  if (v == "i64") return I64;
  if (v == "u8" ) return U8;
  if (v == "u16") return U16;
  if (v == "u32") return U32;
  if (v == "u64") return U64;
  if (v == "f16") return F16;
  if (v == "f32") return F32;
  if (v == "f64") return F64;
  throw Exception("unknown tensor type");
}
size_t Value::Tensor::CountSamples(const std::vector<size_t>& dim) {
  if (dim.size() == 0) {
    throw Exception("empty dimension");
  }
  if (dim.end() != std::find(dim.begin(), dim.end(), 0)) {
    throw Exception("dimension has zero");
  }

  size_t n = 1;
  for (auto x : dim) {
    if (n >= SIZE_MAX/x) throw Exception("dimension overflow");
    n *= x;
  }
  return n;
}

Value::Tensor::Tensor(Type t, std::vector<size_t>&& d, std::vector<uint8_t>&& b) noexcept :
    type_(t), dim_(std::move(d)), buf_(std::move(b)) {
  buf_.resize(CountSamples(dim_) * (t&0xFF));
}

Value::Tensor Value::Tensor::Deserialize(const msgpack::object& obj) {
  try {
    const auto type = Tensor::ParseType(msgpack::find(obj, "type"s).as<std::string>());

    auto dim = msgpack::find(obj, "dim"s).as<std::vector<size_t>>();
    auto buf = msgpack::find(obj, "buf"s).as<std::vector<uint8_t>>();

    CountSamples(dim);
    return Tensor(type, std::move(dim), std::move(buf));

  } catch (Exception& e) {
    throw DeserializeException("invalid tensor: "+e.msg());
  }
}
void Value::Tensor::Serialize(File::Packer& pk) const noexcept {
  pk.pack_map(3);

  pk.pack("type"s);
  pk.pack(StringifyType(type_));

  pk.pack("dim"s);
  pk.pack(dim_);

  pk.pack("buf"s);
  pk.pack(buf_);
}

}  // namespace kingtaker
