#ifndef XTLUA_SHARED_LUA_HELPERS_H
#define XTLUA_SHARED_LUA_HELPERS_H

#include "lua_helpers.h"

void setup_std_vars(lua_State * L, int debug_proc);
void clear_table(lua_State * L, int index);

int vfmt_pcall(
	lua_State * L,
	int debug_proc,
	bool expects_return_value,
	const char * format,
	va_list args);
int fmt_pcall(
	lua_State * L,
	int debug_proc,
	bool expects_return_value,
	const char * format,
	...);
int fmt_pcall_stdvars(
	lua_State * L,
	int debug_proc,
	bool expects_return_value,
	const char * format,
	...);

extern "C" {
	#define HAVE_XPLMPluginID_tostring
	int _XPLMPluginID_tostring(lua_State * L);

	#define HAVE_XPLMHotKeyID_tostring
	int _XPLMHotKeyID_tostring(lua_State * L);

	#define HAVE_XPLMDataRef_tostring
	int _XPLMDataRef_tostring(lua_State * L);
}

#endif
