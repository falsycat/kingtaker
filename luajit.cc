#include "kingtaker.hh"

#include <lua.hpp>


namespace kingtaker {
namespace {

static lua_State* L = nullptr;

bool InitLuaJIT() {
  assert(!L);
  L = luaL_newstate();
  return !!L;
}
void DeinitLuaJIT() {
  assert(L);
  lua_close(L);
}



}
}  // namespace kingtaker
