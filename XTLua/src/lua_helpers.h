//
//  lua_helpers.h
//  xlua
//
//  Created by Benjamin Supnik on 4/13/16.
//
//	Copyright 2016, Laminar Research
//	This source code is licensed under the MIT open source license.
//	See LICENSE.txt for the full terms of the license.

#ifndef lua_helpers_h
#define lua_helpers_h
#include <stdarg.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
};

// Calls the lua func at the stack top with "fmt" args, passed as var-args.
// Returns the lua error if there is one or 0 if success; lua error message
// is printed automagically.
// i - int passed as number
int fmt_pcall(lua_State * L, int dbg, const char * fmt, ...);
int vfmt_pcall(lua_State * L, int dbg, const char * fmt, va_list va);
int fmt_pcall_stdvars(lua_State * L, int dbg, const char * fmt, ...);
int fmt_pcall(lua_State * L, int dbg, bool expects_return_value,
	const char * fmt, ...);
int vfmt_pcall(lua_State * L, int dbg, bool expects_return_value,
	const char * fmt, va_list va);
int fmt_pcall_stdvars(lua_State * L, int dbg, bool expects_return_value,
	const char * fmt, ...);
void setup_std_vars(lua_State * L, int dbg);
void set_stdvars_direct_sdk(lua_State * L, bool direct_sdk);
void clear_table(lua_State * L, int idx);
int lua_pushtraceback(lua_State * L);
#endif /* lua_helpers_h */
