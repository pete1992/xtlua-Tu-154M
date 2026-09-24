//
//  lua_helpers.cpp
//  xlua
//
//  Created by Benjamin Supnik on 4/13/16.
//
//	Copyright 2016, Laminar Research
//	This source code is licensed under the MIT open source license.
//	See LICENSE.txt for the full terms of the license.

#include "lua_helpers.h"
#include <string.h>
#include <stdarg.h>
#include <XPLMDataAccess.h>
#include <XPLMDisplay.h>
#include <XPLMPlugin.h>
#include <XPLMUtilities.h>
#include "xpmtdatarefs.h"
#include "shared_xpfuncs.h"
extern XPLMDataRef				g_replay_active;
extern XPLMDataRef				g_sim_period;

namespace {

// Presence means this state belongs to XTLua. Its boolean value is the
// immutable host decision between worker-buffered and direct SDK callbacks.
static char s_stdvars_runtime_registry_key;

} // namespace

#if 0
int validate_args(lua_State * L, const char * fmt)
{
	if(strlen(fmt) != lua_gettop(L))
	{
		printf("Wrong numer of args: expected %d, got %d\n", (int) strlen(fmt), lua_gettop(L));
		return 0;
	}
	
	int i = 1;
	while(*fmt)
	{
		switch(*fmt) {
		case 's':
			if (!lua_isstring(L, i))
			{
				printf("Argument %d should be a string.\n", i);
				return 0;
			}
			break;
		case 'n':
			if (!lua_isnumber(L, i))
			{
				printf("Argument %d should be a number.\n", i);
				return 0;
			}
			break;
		case 't':
			if (!lua_istable(L, i))
			{
				printf("Argument %d should be a table.\n", i);
				return 0;
			}
			break;
		case 'p':
			if (!lua_islightuserdata(L, i))
			{
				printf("Argument %d should be a command or dataref.\n", i);
				return 0;
			}
			break;
		case 'f':
			if (!lua_isfunction(L, i) && !lua_isnil(L, i))
			{
				printf("Argument %d should be a command or dataref.\n", i);
				return 0;
			}
			break;		
		}
		++fmt;
		++i;
	}
	
	return 1;
}
#endif

static int traceback(lua_State * L)
{
	luaL_traceback(L, L, lua_tostring(L, -1), 2);

	std::string err = std::string(lua_tostring(L, -1)) + "\n";
    //XPLMDebugString(err.c_str());
	lua_getfield(L, LUA_GLOBALSINDEX, "debug");
	lua_getfield(L, -1, "traceback");
	lua_pushvalue(L, 1);
	lua_pushinteger(L, 1);
	lua_call(L,2,1);
	
//	lua_getfield(L, LUA_GLOBALSINDEX, "STP");
//	lua_getfield(L, -1, "stacktrace");
//	lua_pushvalue(L, 1);
//	lua_pushinteger(L, 2);
//	lua_call(L,2,1);
//
	return 1;
}

int lua_pushtraceback(lua_State * L)
{
	lua_pushcfunction(L, traceback);
	return lua_gettop(L);
}

void setup_std_vars(lua_State * L, int dbg)
{
	lua_pushlightuserdata(L, &s_stdvars_runtime_registry_key);
	lua_rawget(L, LUA_REGISTRYINDEX);
	const bool runtime_is_owned = lua_isboolean(L, -1);
	bool use_direct_sdk = runtime_is_owned && lua_toboolean(L, -1) != 0;
	lua_pop(L, 1);

	// lua_helpers is also used by the separate generated-glue host, whose
	// states do not have an XTLua runtime marker. Preserve that host's public
	// version fallback, but never let an owned xtlua_worker state opt itself into
	// direct main-thread SDK calls by overwriting a Lua global.
	if(!runtime_is_owned)
	{
		lua_getglobal(L, "XLuaMajorVersion");
		use_direct_sdk = lua_isnumber(L, -1) && lua_tointeger(L, -1) >= 2;
		lua_pop(L, 1);
	}

	if(use_direct_sdk)
	{
		lua_pushnumber(L, g_sim_period ? XPLMGetDataf(g_sim_period) : 0.02);
		lua_setglobal(L, "SIM_PERIOD");
		lua_pushboolean(L,
			g_replay_active ? XPLMGetDatai(g_replay_active) != 0 : 0);
		lua_setglobal(L, "IN_REPLAY");
		return;
	}

	lua_getfield(L, LUA_GLOBALSINDEX, "setup_callback_var");
	if(lua_isfunction(L, -1))
		fmt_pcall(L, dbg, false, "sf", "SIM_PERIOD", 0.02);
	else
		lua_pop(L, 1);
}

void set_stdvars_direct_sdk(lua_State * L, bool direct_sdk)
{
	if(L == NULL)
		return;
	lua_pushlightuserdata(L, &s_stdvars_runtime_registry_key);
	lua_pushboolean(L, direct_sdk ? 1 : 0);
	lua_rawset(L, LUA_REGISTRYINDEX);
}

int fmt_pcall(lua_State * L, int dbg, const char * fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	int r = vfmt_pcall(L, dbg, false, fmt, va);
	va_end(va);
	return r;
}

int vfmt_pcall(lua_State * L, int dbg, const char * fmt, va_list va)
{
	return vfmt_pcall(L, dbg, false, fmt, va);
}

int fmt_pcall(
	lua_State * L,
	int dbg,
	bool expects_return_value,
	const char * fmt,
	...)
{
	va_list va;
	va_start(va, fmt);
	const int result = vfmt_pcall(L, dbg, expects_return_value, fmt, va);
	va_end(va);
	return result;
}

int vfmt_pcall(
	lua_State * L,
	int dbg,
	bool expects_return_value,
	const char * fmt,
	va_list va)
{
	const char * f = fmt;
	int count = 0;
	while(*f)
	{
		switch(*f) {
		case 'f':
		case 'd':
			lua_pushnumber(L, va_arg(va, double));
			break;		
		case 'i':
			lua_pushinteger(L, va_arg(va, int));
			break;
		case 'b':
			lua_pushboolean(L, va_arg(va, int));
			break;
		case 's':
			lua_pushstring(L, va_arg(va, const char *));
			break;
		case 'n':
			lua_pushnil(L);
			break;
		case 'r':
			lua_rawgeti(L, LUA_REGISTRYINDEX, va_arg(va, int));
			break;
		case 'u':
		default:
			xlua_pushuserdata<void *>(L, va_arg(va, void *));
			break;
		}
		++f;
		++count;
	}
	int e = lua_pcall(L, count, expects_return_value ? 1 : 0, dbg);
	if(e != 0)
	{
		const char * message = lua_tostring(L, -1);
		log_message(L, "Lua callback failed (%d): %s\n",
			e,
			message ? message : "(non-string error)");
		lua_pop(L, 1);
	}
	return e;
}

int fmt_pcall_stdvars(lua_State * L, int dbg, const char * fmt, ...)
{
	setup_std_vars(L, dbg);
	va_list va;
	va_start(va, fmt);
	int r = vfmt_pcall(L, dbg, false, fmt, va);
	va_end(va);
	return r;
}

int fmt_pcall_stdvars(
	lua_State * L,
	int dbg,
	bool expects_return_value,
	const char * fmt,
	...)
{
	setup_std_vars(L, dbg);
	va_list va;
	va_start(va, fmt);
	const int result = vfmt_pcall(
		L,
		dbg,
		expects_return_value,
		fmt,
		va);
	va_end(va);
	return result;
}

void clear_table(lua_State * L, int idx)
{
	if(idx < 0)
		idx = lua_gettop(L) + idx + 1;
	if(idx <= 0 || !lua_istable(L, idx))
		return;

	lua_pushnil(L);
	while(lua_next(L, idx) != 0)
	{
		lua_pop(L, 1);
		lua_pushvalue(L, -1);
		lua_pushnil(L);
		lua_settable(L, idx);
	}
}

extern "C" int _XPLMPluginID_tostring(lua_State * L)
{
	const XPLMPluginID plugin =
		xlua_checkuserdata<XPLMPluginID>(L, 1, "Expected XPLMPluginID");
	char name[256] = {};
	XPLMGetPluginInfo(plugin, name, nullptr, nullptr, nullptr);
	lua_pushstring(L, name);
	return 1;
}

extern "C" int _XPLMHotKeyID_tostring(lua_State * L)
{
	const XPLMHotKeyID hotkey =
		xlua_checkuserdata<XPLMHotKeyID>(L, 1, "Expected XPLMHotKeyID");
	char name[256] = {};
	XPLMGetHotKeyInfo(hotkey, nullptr, nullptr, name, nullptr);
	lua_pushstring(L, name);
	return 1;
}

extern "C" int _XPLMDataRef_tostring(lua_State * L)
{
	const XPLMDataRef dataref =
		xlua_checkuserdata<XPLMDataRef>(L, 1, "Expected XPLMDataRef");
#if defined(XPLM400)
	XPLMDataRefInfo_t info = {};
	info.structSize = sizeof(info);
	XPLMGetDataRefInfo(dataref, &info);
	lua_pushstring(L, info.name ? info.name : "");
#else
	lua_pushfstring(L, "XPLMDataRef: %p", dataref);
#endif
	return 1;
}
