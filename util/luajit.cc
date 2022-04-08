#include "util/luajit.hh"


namespace kingtaker::luajit {

static int L_GetValueType(lua_State* L) {
  auto v = Device::GetObj<Value>(L, 1, "Value");
  lua_pushstring(L, v->StringifyType());
  return 1;
}
template <typename T>
static int L_GetValueAs(lua_State* L) {
  auto v = Device::GetObj<Value>(L, 1, "Value");
  try {
    if constexpr (std::is_same<T, Value::Integer>::value) {
      lua_pushinteger(L, static_cast<Value::Integer>(v->integer()));
      return 1;

    } else if constexpr (std::is_same<T, Value::Scalar>::value) {
      lua_pushnumber(L, static_cast<Value::Scalar>(v->scalar()));
      return 1;

    } else if constexpr (std::is_same<T, Value::Boolean>::value) {
      lua_pushboolean(L, v->boolean());
      return 1;

    } else if constexpr (std::is_same<T, Value::String>::value) {
      const auto& str = v->string();
      lua_pushlstring(L, str.c_str(), str.size());
      return 1;

    } else {
      []<bool f = false>() { static_assert(f, "unknown type"); }();
    }
  } catch (Exception&) {
    lua_pushnil(L);
    return 1;
  }
}
template <typename T>
static int L_PushValue(lua_State* L) {
  if constexpr (std::is_same<T, Value::Pulse>::value) {
    Device::PushValue(L, Value::Pulse());

  } else if constexpr (std::is_same<T, Value::Integer>::value) {
    Device::PushValue(L, static_cast<Value::Integer>(luaL_checkinteger(L, 1)));

  } else if constexpr (std::is_same<T, Value::Scalar>::value) {
    Device::PushValue(L, static_cast<Value::Scalar>(luaL_checknumber(L, 1)));

  } else if constexpr (std::is_same<T, Value::Boolean>::value) {
    Device::PushValue(L, static_cast<Value::Boolean>(!!luaL_checkint(L, 1)));

  } else if constexpr (std::is_same<T, Value::String>::value) {
    Device::PushValue(L, std::string(luaL_checkstring(L, 1)));

  } else {
    []<bool f = false>() { static_assert(f, "unknown type"); }();
  }
  return 1;
}


void Device::Main() noexcept {
  std::unique_lock<std::mutex> k(mtx_);
  while (alive_) {
    cv_.wait(k);
    for (;;) {
      if (cmds_.empty()) break;
      if (!L) {
        k.unlock();
        if (!SetUp()) break;
        k.lock();
      }

      auto cmd = std::move(cmds_.front());
      cmds_.pop_front();

      // clear stack and execute the command
      k.unlock();
      lua_settop(L, 0);
      cmd(L);
      k.lock();
    }
  }
  if (L) lua_close(L);
}
bool Device::SetUp() noexcept {
  L = luaL_newstate();
  if (!L) return false;

  // create immutable table for sandboxing
  lua_createtable(L, 0, 0);
    if (luaL_newmetatable(L, "ImmTable")) {
      lua_pushvalue(L, LUA_GLOBALSINDEX);
      lua_setfield(L, -2, "__index");

      lua_pushcfunction(L, [](auto L) { return luaL_error(L, "global is immutable"); });
      lua_setfield(L, -2, "__newindex");
    }
    lua_setmetatable(L, -2);
  imm_table_ = luaL_ref(L, LUA_REGISTRYINDEX);

  // std table
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  lua_createtable(L, 0, 0);
    lua_createtable(L, 0, 0);
      lua_pushcfunction(L, L_PushValue<Value::Pulse>);
      lua_setfield(L, -2, "pulse");

      lua_pushcfunction(L, L_PushValue<Value::Integer>);
      lua_setfield(L, -2, "integer");

      lua_pushcfunction(L, L_PushValue<Value::Scalar>);
      lua_setfield(L, -2, "scalar");

      lua_pushcfunction(L, L_PushValue<Value::Boolean>);
      lua_setfield(L, -2, "boolean");

      lua_pushcfunction(L, L_PushValue<Value::String>);
      lua_setfield(L, -2, "string");
    lua_setfield(L, -2, "value");
  lua_setfield(L, -2, "std");

  return true;
}

void Device::PushValue(lua_State* L, const Value& v) noexcept {
  NewObjWithoutMeta<Value>(L, v);
  if (luaL_newmetatable(L, "Value")) {
    lua_createtable(L, 0, 0);
      lua_pushcfunction(L, L_GetValueType);
      lua_setfield(L, -2, "type");

      lua_pushcfunction(L, L_GetValueAs<Value::Integer>);
      lua_setfield(L, -2, "integer");

      lua_pushcfunction(L, L_GetValueAs<Value::Scalar>);
      lua_setfield(L, -2, "scalar");

      lua_pushcfunction(L, L_GetValueAs<Value::String>);
      lua_setfield(L, -2, "string");
    lua_setfield(L, -2, "__index");

    PushObjDeleter<Value>(L);
    lua_setfield(L, -2, "__gc");
  }
  lua_setmetatable(L, -2);
}

int Device::SandboxCall(lua_State*, int narg, int nret) const noexcept {
  static constexpr size_t kSandboxInstructionLimit = 10000000;

  // set instruction limit
  static const auto hook = [](auto L, auto) {
    luaL_error(L, "reached instruction limit (<=1e8)");
  };
  lua_sethook(L, hook, LUA_MASKCOUNT, kSandboxInstructionLimit);

  // set env to empty table
  lua_rawgeti(L, LUA_REGISTRYINDEX, imm_table_);
  lua_setfenv(L, -narg-2);

  // call
  return lua_pcall(L, narg, nret, 0);
}

}  // namespace kingtaker::luajit
