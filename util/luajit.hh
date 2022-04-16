#include "kingtaker.hh"

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <lua.hpp>

#include "util/value.hh"


namespace kingtaker::luajit {

class Device final {
 public:
  using Command = std::function<void(lua_State* L)>;

  class RuntimeException : public HeavyException {
   public:
    RuntimeException(std::string_view msg, Loc loc = Loc::current()) noexcept :
        HeavyException(msg, loc) {
    }
    std::string Stringify() const noexcept {
      return "[LuaJIT Exception]\n"s+Exception::Stringify();
    }
  };

  Device() noexcept {
    th_ = std::thread([this]() { Main(); });
  }
  ~Device() noexcept {
    alive_ = false;
    cv_.notify_all();
    th_.join();
  }

  void Queue(Command&& cmd) noexcept {
    std::unique_lock<std::mutex> k(mtx_);
    cmds_.push_back(std::move(cmd));
    cv_.notify_all();
  }


  static void PushValue(lua_State* L, const Value& v) noexcept;

  // the first arg is not used but necessary
  // to make it ensure to be called from lua thread
  int SandboxCall(lua_State*, int narg, int nret) const noexcept;

  // create and push userdata that holds an instance of T
  // the destructor is guaranteed to be called by GC
  template <typename T, typename... Args>
  static T* NewObj(lua_State* L, Args&&... args) {
    static const auto kName = "Obj_"s+typeid(T).name();

    auto ret = NewObjWithoutMeta<T>(std::forward<Args>(args)...);
    if (luaL_newmetatable(L, kName.c_str())) {
      PushObjDeleter<T>(L);
      lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
    return ret;
  }
  template <typename T, typename... Args>
  static T* NewObjWithoutMeta(lua_State* L, Args&&... args) {
    auto ret = std::make_unique<T>(std::forward<Args>(args)...);
    auto ptr = (T**) lua_newuserdata(L, sizeof(T*));
    return *ptr = ret.release();
  }
  template <typename T>
  static void PushObjDeleter(lua_State* L) {
    static const auto gc = [](auto L) {
      auto udata = *(T**) lua_touserdata(L, 1);
      delete udata;
      return 0;
    };
    lua_pushcfunction(L, gc);
  }
  template <typename T>
  static T* GetObj(lua_State* L, int index, const char* name = nullptr) {
    static const auto kName = "Obj_"s+typeid(T).name();
    if (!name) name = kName.c_str();
    return *(T**) luaL_checkudata(L, index, name);
  }
  template <typename T>
  static T* GetObjIf(lua_State* L, int index, const char* name = nullptr) {
    if (!lua_isuserdata(L, index)) return nullptr;

    auto ptr = lua_touserdata(L, index);
    if (!name) return *(T**) ptr;

    lua_getmetatable(L, index);
    lua_getfield(L, LUA_REGISTRYINDEX, name);
    auto ret = lua_rawequal(L, -1, -2)? *(T**) ptr: nullptr;
    lua_pop(L, 2);
    return ret;
  }

 private:
  lua_State* L = nullptr;

  // thread and command queue
  std::thread             th_;
  std::deque<Command>     cmds_;
  std::mutex              mtx_;
  std::condition_variable cv_;

  std::atomic<bool> alive_ = true;


  // lua values (modified only from lua thread)
  int imm_table_ = LUA_REFNIL;


  void Main() noexcept;
  bool SetUp() noexcept;
};


class Obj final : public Value::Data {
 public:
  static inline const char* kName = "kingtaker::luajit::Obj";

  static std::shared_ptr<Obj> PopAndCreate(Device* dev, lua_State* L) noexcept {
    return std::make_shared<Obj>(dev, luaL_ref(L, LUA_REGISTRYINDEX));
  }

  Obj(Device* dev, int reg) noexcept :
      Data(kName), dev_(dev), reg_(reg) {
  }
  ~Obj() noexcept {
    dev_->Queue([i = reg_](auto L) { luaL_unref(L, LUA_REGISTRYINDEX, i); });
  }

  int reg() const noexcept { return reg_; }

 private:
  Device* dev_;

  int reg_;
};

}  // namespace kingtaker::luajit
