#include "xlua2_host.h"

#include "shared_xpfuncs.h"
#include "xptimers.h"

#include <cmath>

namespace {

xlua2_timer * check_timer(lua_State * L, int argument)
{
	return xlua_checkuserdata<xlua2_timer *>(
		L,
		argument,
		"expected XLua timer");
}

double check_finite_number(lua_State * L, int argument, const char * message)
{
	const double value = luaL_checknumber(L, argument);
	if(!std::isfinite(value))
		luaL_argerror(L, argument, message);
	return value;
}

} // namespace

extern "C" int XLuaCreateTimer(lua_State * L)
{
	luaL_checktype(L, 1, LUA_TFUNCTION);
	xlua2_timer * timer = xlua2_create_timer(L, 1);
	if(timer == NULL)
	{
		return luaL_error(
			L,
			"XLuaCreateTimer: a timer already exists for this callback");
	}

	xlua_pushuserdata<xlua2_timer *>(L, timer);
	return 1;
}

extern "C" int XLuaRunTimer(lua_State * L)
{
	xlua2_timer * timer = check_timer(L, 1);
	const double delay = check_finite_number(
		L,
		2,
		"timer delay must be a finite number");
	const double repeat = check_finite_number(
		L,
		3,
		"timer period must be a finite number");

	if(!xlua2_run_timer(L, timer, delay, repeat))
		return luaL_argerror(L, 1, "invalid timer for this Lua state");
	return 0;
}

extern "C" int XLuaFindTimer(lua_State * L)
{
	luaL_checktype(L, 1, LUA_TFUNCTION);
	xlua2_timer * timer = xlua2_find_timer(L, 1);
	if(timer == NULL)
	{
		lua_pushnil(L);
		return 1;
	}

	xlua_pushuserdata<xlua2_timer *>(L, timer);
	return 1;
}

extern "C" int XLuaIsTimerScheduled(lua_State * L)
{
	xlua2_timer * timer = check_timer(L, 1);
	lua_pushboolean(L, xlua2_is_timer_scheduled(L, timer));
	return 1;
}

extern "C" int XLuaGetTimerRemaining(lua_State * L)
{
	xlua2_timer * timer = check_timer(L, 1);
	lua_pushnumber(L, xlua2_get_timer_remaining(L, timer));
	return 1;
}

extern "C" int XLuaReloadOnFlightChange(lua_State * L)
{
	(void) L;
	xlua2_host_reload_on_flight_change();
	return 0;
}

extern "C" int XLuaCreateImguiWindow(lua_State * L)
{
	return luaL_error(
		L,
		"XLuaCreateImguiWindow is not supported by this XTLua build");
}

extern "C" int XLuaDestroyImguiWindow(lua_State * L)
{
	return luaL_error(
		L,
		"XLuaDestroyImguiWindow is not supported by this XTLua build");
}
