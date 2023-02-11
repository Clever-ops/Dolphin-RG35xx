#include "Core/Lua/LuaHelperClasses/LuaColonCheck.h"

#include <fmt/format.h>

namespace Lua
{
void LuaColonOperatorTypeCheck(lua_State* lua_state, const char* function_name,
                               const char* example_call)
{
  if (lua_type(lua_state, 1) != LUA_TUSERDATA)
  {
    luaL_error(
        lua_state,
        fmt::format("Error: User attempted to call {} function using the dot operator. Please use the colon operator instead like this: '{}'", function_name, example_call).c_str());
  }
}

}  // namespace Lua
