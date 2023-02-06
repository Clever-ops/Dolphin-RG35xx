#pragma once
#include <string>
extern "C" {
#include "src/lapi.h"
#include "src/lua.h"
#include "src/lua.hpp"
#include "src/luaconf.h"
}
#include "../LuaHelperClasses/LuaColonCheck.h"

namespace Lua
{
namespace LuaRegisters
{
void InitLuaRegistersFunctions(lua_State* luaState, const std::string& luaApiVersion);
int getRegister(lua_State* luaState);
int getRegisterAsUnsignedByteArray(lua_State* luaState);
int getRegisterAsSignedByteArray(lua_State* luaState);
int setRegister(lua_State* luaState);
int setRegisterFromByteArray(lua_State* luaState);
}
}
