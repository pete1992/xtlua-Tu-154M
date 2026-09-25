//
//  xpfuncs.cpp
//  xlua
//
//  Created by Ben Supnik on 3/19/16.
//
//	Copyright 2016, Laminar Research
//	This source code is licensed under the MIT open source license.
//	See LICENSE.txt for the full terms of the license.
// xTLua
// Modified by Mark Parker on 04/19/2020

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <climits>
#include <cmath>
#include "xpfuncs.h"
#include "lua_helpers.h"
#include "xpdatarefs.h"
#include "xpcommands.h"
#include "xptimers.h"
#include "module.h"
#include "shared_xpfuncs.h"

#include <string.h>
#include <stdio.h>
#include <assert.h>

/*
	TODO: figure out when we have to resync our datarefs
	TODO: what if dref already registered before acf reload?  (maybe no harm?)
	TODO: test x-plane-side string read/write - needs test not at startup

 */



// This is kind of a mess - Lua [annoyingly] doesn't give you a way to store a closure/Lua interpreter function
// in C space.  The hack is to use luaL_ref to fill a new key in the registry table with a copy of ANY value from
// the stack - since this is type agnostic and takes a strong reference it (1) prevents the closure from being 
// garbage collected and (2) works with closures.

// Callback storage shared by xtlua_worker and xtlua_main bindings.
struct xtlua_notify_cb_t {
	lua_State *		L;
	int				slot;
	module *		owner;
};

// Given an interp and a stack arg that is a lua function/closure,
// this routine stashes a strong ref to the closure in the registry,
// allcoates a callback struct and stashes the slot and interp in the
// CB struct.  This CB struct is a single C ptr that we can use to 
// reconstruct the closure from C land.
//
// If the closure is actually nil, we return NULL and allocate nothing.
// The memory is tracked by the interp's module and is collected for us
// at shutdown.
xtlua_notify_cb_t * wrap_lua_func(lua_State * L, int idx)
{
	if(lua_isnil(L, idx))
	{
		luaL_argerror(L, idx, "nil not allowed for callback");
		return NULL;
	}
	luaL_checktype(L, idx, LUA_TFUNCTION);
	
	module * me = module::module_from_interp(L);
	if(me == NULL)
	{
		luaL_error(L, "module context unavailable");
		return NULL;
	}
	xtlua_notify_cb_t * cb = (xtlua_notify_cb_t *) me->module_alloc_tracked(sizeof(xtlua_notify_cb_t));
	if(cb == NULL)
	{
		luaL_error(L, "unable to allocate callback state");
		return NULL;
	}
	cb->L = L;
	cb->owner = me;
	lua_pushvalue (L, idx);
	cb->slot = luaL_ref(L, LUA_REGISTRYINDEX);		
	return cb;
}

xtlua_notify_cb_t * wrap_lua_func_nil(lua_State * L, int idx)
{
	if(lua_isnil(L,idx))
	{
		return NULL;
	}
	return wrap_lua_func(L, idx);
}

// Given a void * that is really a CB struct, this routine either
// pushes the lua function onto the stack (so that we can then push 
// args and pcall) or returns 0 if we should not call because the CB is
// nil or borked.
lua_State * setup_lua_callback(void * ref)
{
	if(ref == NULL) 
		return NULL;
	xtlua_notify_cb_t * cb = (xtlua_notify_cb_t *) ref;
	// Constructor-failure teardown may already have closed cb->L while its
	// module and tracked callback record remain alive until host cleanup.
	if(cb->owner == NULL || cb->owner->is_closing())
		return NULL;
	lua_rawgeti (cb->L, LUA_REGISTRYINDEX, cb->slot);
	if(!lua_isfunction(cb->L, -1))
	{
		printf("ERROR: we did not persist a closure?!?");
		lua_pop(cb->L, 1);
		return 0;
	}
	return cb->L;
}

template <typename T>
T * luaL_checkuserdata(lua_State * L, int narg, const char * msg)
{
	T * ret = (T*) lua_touserdata(L, narg);
	if(ret == NULL)
		luaL_argerror(L, narg, msg);
	return ret;	
}

static int luaL_check_array_index(lua_State * L, int narg)
{
	double value = luaL_checknumber(L, narg);
	if(!std::isfinite(value) || value < 0.0 || value > INT_MAX || std::floor(value) != value)
		return luaL_argerror(L, narg, "array index must be a non-negative integer");
	return static_cast<int>(value);
}

static double luaL_check_finite_time(lua_State * L, int narg)
{
	const double value = luaL_checknumber(L, narg);
	if(!std::isfinite(value))
		luaL_argerror(L, narg, "timer delay/repeat must be finite");
	return value;
}

//----------------------------------------------------------------
// MISC
//----------------------------------------------------------------

static int XLuaGetCode(lua_State * L)
{
	module * me = module::module_from_interp(L);
	if(me == NULL)
		return luaL_error(L, "module context unavailable");
	
	const char * name = luaL_checkstring(L, 1);
	
	int result = me->load_module_relative_path(name);
	
	if(result)
	{
		const char * err_msg = luaL_checkstring(L,-1);
		printf("%s: %s", name, err_msg);
	}
	
	return 1;
}


//----------------------------------------------------------------
// DATAREFS
//----------------------------------------------------------------

// XPLMFindDataRef "foo" -> dref
static int XLuaFindDataRef(lua_State * L)
{
	const char * name = luaL_checkstring(L, -1);

	xlua_dref * r = xlua_find_dref(name);
	assert(r);
	
	lua_pushlightuserdata(L, r);
	return 1;
}
static int XTLuaFindDataRef(lua_State * L)
{
	const char * name = luaL_checkstring(L, -1);

	xtlua_dref * r = xtlua_find_dref(name);
	assert(r);
	
	lua_pushlightuserdata(L, r);
	return 1;
}
static void xlua_notify_helper(xlua_dref * who, void * ref)
{
	lua_State * L = setup_lua_callback(ref);
	if(L)
	{
		fmt_pcall_stdvars(L, 0, "");
	}
}

// XPLMCreateDataRef name "array[4]" "yes" func -> dref
static int XLuaCreateDataRef(lua_State * L)
{
	const char * name = luaL_checkstring(L, 1);
	const char * typestr = luaL_checkstring(L,2);
	const char * writable = luaL_checkstring(L,3);	
	
	if(strlen(name) == 0)
		return luaL_argerror(L, 1, "dataref name must not be an empty string.");

	int my_writeable;
	if(strcmp(writable,"yes")==0)
		my_writeable = 1;
	else if (strcmp(writable,"no")==0)
		my_writeable = 0;
	else 
		return luaL_argerror(L, 3, "writable must be 'yes' or 'no'");
	
	xtlua_dref_type my_type = xlua_none;
	int my_dim = 1;
	const char * c = typestr;
	if(strcmp(c,"string") == 0)
		my_type = xlua_string;
	else if(strcmp(c,"number")==0)
		my_type = xlua_number;
	else if (strncmp(c,"array[",6) == 0)
	{
		char * end = NULL;
		errno = 0;
		long dim = std::strtol(c + 6, &end, 10);
		if(errno == ERANGE || end == c + 6 || *end != ']' || end[1] != '\0' || dim <= 0 || dim > INT_MAX)
			return luaL_argerror(L, 2, "array type must be array[n] with n greater than zero");
		my_dim = static_cast<int>(dim);
		my_type = xlua_array;
	}
	else
		return luaL_argerror(L, 2, "Type must be number, string, or array[n]");
	
	xtlua_notify_cb_t * cb = wrap_lua_func_nil(L, 4);
	xlua_dref * r = xlua_create_dref(
							name,
							my_type,
							my_dim,
							my_writeable,
							(my_writeable && cb) ? xlua_notify_helper : NULL,
							cb);							
	assert(r);
	
	lua_pushlightuserdata(L, r);
	return 1;
	
}

// dref -> "array[4]"
static int XTLuaGetDataRefType(lua_State * L)
{
	xtlua_dref * d = luaL_checkuserdata<xtlua_dref>(L,1,"expected dataref");

	xtlua_dref_type dt = xtlua_dref_get_type(d);
	
	switch(dt) {
	case xlua_none:
		lua_pushstring(L, "none");
		break;
	case xlua_number:
		lua_pushstring(L, "number");
		break;
	case xlua_array:
		{
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
			char buf[256];
			sprintf(buf,"array[%d]",xtlua_dref_get_dim(d));
			lua_pushstring(L,buf);

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
		}
		break;
	case xlua_string:
		lua_pushstring(L, "string");
		break;
	}	
	return 1;
}
static int XLuaGetDataRefType(lua_State * L)
{
	xlua_dref * d = luaL_checkuserdata<xlua_dref>(L,1,"expected dataref");

	xtlua_dref_type dt = xlua_dref_get_type(d);
	
	switch(dt) {
	case xlua_none:
		lua_pushstring(L, "none");
		break;
	case xlua_number:
		lua_pushstring(L, "number");
		break;
	case xlua_array:
		{
			char buf[256];
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
			sprintf(buf,"array[%d]",xlua_dref_get_dim(d));

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
			lua_pushstring(L,buf);
		}
		break;
	case xlua_string:
		lua_pushstring(L, "string");
		break;
	}	
	return 1;
}
// XPLMGetNumber dref -> value
static int XTLuaGetNumber(lua_State * L)
{
	xtlua_dref * d = luaL_checkuserdata<xtlua_dref>(L,1,"expected dataref");
	
	lua_pushnumber(L, xtlua_dref_get_number(d));
	return 1;	
}

// XPLMSetNumber dref value
static int XTLuaSetNumber(lua_State * L)
{
	xtlua_dref * d = luaL_checkuserdata<xtlua_dref>(L,1,"expected dataref");
	double v = luaL_checknumber(L, 2);
	
	xtlua_dref_set_number(d,v);
	return 0;	
}
static int XLuaExistingDataRef(lua_State * L)
{
	const char * s = luaL_checkstring(L, 1);
	XPLMDataRef dRefcheckother = XPLMFindDataRef(s);
    if(!dRefcheckother){
        printf("no %s\n",s);
		lua_pushnumber(L, 0);
	}
    else {
        printf("existing %s\n",s); 
		lua_pushnumber(L, 1);
	}
    return 1;
}
static int XLuaGetNumber(lua_State * L)
{
	xlua_dref * d = luaL_checkuserdata<xlua_dref>(L,1,"expected dataref");
	
	lua_pushnumber(L, xlua_dref_get_number(d));
	return 1;	
}

// XPLMSetNumber dref value
static int XLuaSetNumber(lua_State * L)
{
	xlua_dref * d = luaL_checkuserdata<xlua_dref>(L,1,"expected dataref");
	double v = luaL_checknumber(L, 2);
	xlua_dref_set_number(d,v);
	return 0;	
}
// XPLMGetArray dref idx -> value
static int XTLuaGetArray(lua_State * L)
{
	xtlua_dref * d = luaL_checkuserdata<xtlua_dref>(L,1,"expected dataref");
	int idx = luaL_check_array_index(L, 2);
	lua_pushnumber(L, xtlua_dref_get_array(d,idx));
	return 1;	
}

// XPLMSetArray dref dix value
static int XTLuaSetArray(lua_State * L)
{
	xtlua_dref * d = luaL_checkuserdata<xtlua_dref>(L,1,"expected dataref");
	int idx = luaL_check_array_index(L, 2);
	double v = luaL_checknumber(L, 3);
	
	xtlua_dref_set_array(d,idx,v);
	return 0;		
}

static int array_length(xtlua_dref * d) { return xtlua_dref_get_dim(d); }
static int array_length(xlua_dref * d) { return xlua_dref_get_dim(d); }
static xtlua_dref_type array_type(xtlua_dref * d) { return xtlua_dref_get_type(d); }
static xtlua_dref_type array_type(xlua_dref * d) { return xlua_dref_get_type(d); }
static std::vector<double> array_read(xtlua_dref * d, int offset, int count)
{ return xtlua_dref_get_array_values(d, offset, count); }
static std::vector<double> array_read(xlua_dref * d, int offset, int count)
{ return xlua_dref_get_array_values(d, offset, count); }
static int array_write(xtlua_dref * d, const std::vector<double>& values, int offset)
{ return xtlua_dref_set_array_values(d, values, offset); }
static int array_write(xlua_dref * d, const std::vector<double>& values, int offset)
{ return xlua_dref_set_array_values(d, values, offset); }

template <typename Dref>
static int get_array_length(lua_State * L)
{
	Dref * d = luaL_checkuserdata<Dref>(L, 1, "expected dataref");
	if(array_type(d) != xlua_array)
		return luaL_argerror(L, 1, "expected array dataref");
	lua_pushinteger(L, array_length(d));
	return 1;
}

// Shared validation, separate C++ overloads: workers never select SDK functions.
// Offsets are zero-based; Lua bulk value tables are dense and one-based.
template <typename Dref>
static int get_array_values(lua_State * L)
{
	Dref * d = luaL_checkuserdata<Dref>(L, 1, "expected dataref");
	if(array_type(d) != xlua_array)
		return luaL_argerror(L, 1, "expected array dataref");
	const int dim = array_length(d);
	const int offset = lua_isnoneornil(L, 2) ? 0 : luaL_check_array_index(L, 2);
	if(offset > dim)
		return luaL_argerror(L, 2, "array offset exceeds length");
	const int count = lua_isnoneornil(L, 3) ? dim - offset : luaL_check_array_index(L, 3);
	if(count > dim - offset)
		return luaL_argerror(L, 3, "array range exceeds length");
	bool complete = false;
	{
		const std::vector<double> values = array_read(d, offset, count);
		complete = values.size() == static_cast<size_t>(count);
		if(complete)
		{
			lua_createtable(L, count, 0);
			for(int i = 0; i < count; ++i)
			{
				lua_pushnumber(L, values[static_cast<size_t>(i)]);
				lua_rawseti(L, -2, i + 1);
			}
		}
	}
	if(!complete)
		return luaL_error(L, "array changed while reading snapshot");
	return 1;
}

template <typename Dref>
static int set_array_values(lua_State * L)
{
	Dref * d = luaL_checkuserdata<Dref>(L, 1, "expected dataref");
	if(array_type(d) != xlua_array)
		return luaL_argerror(L, 1, "expected array dataref");
	luaL_checktype(L, 2, LUA_TTABLE);
	const int dim = array_length(d);
	const int offset = lua_isnoneornil(L, 3) ? 0 : luaL_check_array_index(L, 3);
	if(offset > dim)
		return luaL_argerror(L, 3, "array offset exceeds length");
	const size_t count = lua_objlen(L, 2);
	if(count > static_cast<size_t>(dim - offset))
		return luaL_argerror(L, 2, "array range exceeds length");
	// Validate everything before allocating the C++ vector: Lua's argument
	// errors use longjmp and cannot unwind live standard-library containers.
	size_t keys = 0;
	lua_pushnil(L);
	while(lua_next(L, 2) != 0)
	{
		if(lua_type(L, -2) != LUA_TNUMBER || lua_type(L, -1) != LUA_TNUMBER)
			return luaL_argerror(L, 2, "array values must be a dense numeric sequence");
		const double key = lua_tonumber(L, -2);
		if(!std::isfinite(key) || key < 1.0 || key > static_cast<double>(count) || std::floor(key) != key)
			return luaL_argerror(L, 2, "array values must use sequential numeric keys");
		++keys;
		lua_pop(L, 1);
	}
	if(keys != count)
		return luaL_argerror(L, 2, "array values must be a dense numeric sequence");
	int written = 0;
	{
		std::vector<double> values(count);
		for(size_t i = 0; i < count; ++i)
		{
			lua_rawgeti(L, 2, static_cast<int>(i + 1));
			values[i] = lua_tonumber(L, -1);
			lua_pop(L, 1);
		}
		written = array_write(d, values, offset);
	}
	if(written != static_cast<int>(count))
		return luaL_error(L, "array changed while writing snapshot");
	lua_pushinteger(L, written);
	return 1;
}

static int XTLuaGetArrayLength(lua_State * L) { return get_array_length<xtlua_dref>(L); }
static int XTLuaGetArrayValues(lua_State * L) { return get_array_values<xtlua_dref>(L); }
static int XTLuaSetArrayValues(lua_State * L) { return set_array_values<xtlua_dref>(L); }
static int XLuaGetArrayLength(lua_State * L) { return get_array_length<xlua_dref>(L); }
static int XLuaGetArrayValues(lua_State * L) { return get_array_values<xlua_dref>(L); }
static int XLuaSetArrayValues(lua_State * L) { return set_array_values<xlua_dref>(L); }
static int XLuaGetArray(lua_State * L)
{
	xlua_dref * d = luaL_checkuserdata<xlua_dref>(L,1,"expected dataref");
	int idx = luaL_check_array_index(L, 2);
	lua_pushnumber(L, xlua_dref_get_array(d,idx));
	return 1;	
}

// XPLMSetArray dref dix value
static int XLuaSetArray(lua_State * L)
{
	xlua_dref * d = luaL_checkuserdata<xlua_dref>(L,1,"expected dataref");
	int idx = luaL_check_array_index(L, 2);
	double v = luaL_checknumber(L, 3);
	
	xlua_dref_set_array(d,idx,v);
	return 0;		
}
// XPLMGetString dref -> value
static int XTLuaGetString(lua_State * L)
{
	xtlua_dref * d = luaL_checkuserdata<xtlua_dref>(L,1,"expected dataref");
	
	lua_pushstring(L, xtlua_dref_get_string(d).c_str());
	return 1;	
}

// XPLMSetString dref value
static int XTLuaSetString(lua_State * L)
{
	xtlua_dref * d = luaL_checkuserdata<xtlua_dref>(L,1,"expected dataref");
	const char * s = luaL_checkstring(L, 2);
	xtlua_dref_set_string(d,string(s));
	return 0;	
}
static int XLuaGetString(lua_State * L)
{
	xlua_dref * d = luaL_checkuserdata<xlua_dref>(L,1,"expected dataref");
	
	lua_pushstring(L, xlua_dref_get_string(d).c_str());
	return 1;	
}

// XPLMSetString dref value
static int XLuaSetString(lua_State * L)
{
	xlua_dref * d = luaL_checkuserdata<xlua_dref>(L,1,"expected dataref");
	const char * s = luaL_checkstring(L, 2);
	xlua_dref_set_string(d,string(s));
	return 0;	
}
//----------------------------------------------------------------
// COMMANDS
//----------------------------------------------------------------

// XPLMFindCommand name
static int XTLuaFindCommand(lua_State * L)
{
	const char * name = luaL_checkstring(L, 1);
	xtlua_cmd * r = xtlua_find_cmd(name);
	if(!r)
	{
		lua_pushnil(L);
		return 1;
	}
	assert(r);
	
	lua_pushlightuserdata(L, r);
	return 1;
}
static int XLuaFindCommand(lua_State * L)
{
	const char * name = luaL_checkstring(L, 1);
	xlua_cmd * r = xlua_find_cmd(name);
	if(!r)
	{
		lua_pushnil(L);
		return 1;
	}
	assert(r);
	
	lua_pushlightuserdata(L, r);
	return 1;
}
// XPLMCreateCommand name desc
static int XLuaCreateCommand(lua_State * L)
{
	const char * name = luaL_checkstring(L, 1);
	const char * desc = luaL_checkstring(L, 2);

	xlua_cmd * r = xlua_create_cmd(name,desc);
	assert(r);
	
	lua_pushlightuserdata(L, r);
	return 1;
}

static void cmd_cb_helper(xtlua_cmd * cmd, int phase, float elapsed, void * ref)
{
	//printf("xtcmd_cb_helper\n");
	lua_State * L = setup_lua_callback(ref);
	if(L)
	{
		fmt_pcall_stdvars(L, 0, "if", phase, elapsed);
	}
}
static void xlcmd_cb_helper(xlua_cmd * cmd, int phase, float elapsed, void * ref)
{
	//printf("xlcmd_cb_helper\n");
	lua_State * L = setup_lua_callback(ref);
	if(L)
	{
		fmt_pcall_stdvars(L, 0, "if", phase, elapsed);
	}
}
// XPLMReplaceCommand cmd handler
static int XlLuaReplaceCommand(lua_State * L)
{
	
	xlua_cmd * d = luaL_checkuserdata<xlua_cmd>(L,1,"expected command");

	xtlua_notify_cb_t * cb = wrap_lua_func(L, 2);
	
	xlua_cmd_install_handler(d, xlcmd_cb_helper, cb);
	return 0;	
}

static int XLuaReplaceCommand(lua_State * L)
{
	return XlLuaReplaceCommand(L); // Preserve the old misspelled binding as an alias.
}

static int XLuaWrapCommand(lua_State * L)
{
	xlua_cmd * d = luaL_checkuserdata<xlua_cmd>(L, 1, "expected command");
	// Validate both functions before persisting either callback.
	luaL_checktype(L, 2, LUA_TFUNCTION);
	luaL_checktype(L, 3, LUA_TFUNCTION);
	xtlua_notify_cb_t * before = wrap_lua_func(L, 2);
	xtlua_notify_cb_t * after = wrap_lua_func(L, 3);
	xlua_cmd_install_pre_wrapper(d, xlcmd_cb_helper, before);
	xlua_cmd_install_post_wrapper(d, xlcmd_cb_helper, after);
	return 0;
}

static bool xlcmd_filter_helper(xlua_cmd *, void * ref)
{
	lua_State * L = setup_lua_callback(ref);
	if(L == NULL)
		return true;
	// A filter must return synchronously. This helper is registered only by
	// xtlua_main, never by xtlua_worker or its deferred command phase queue.
	const int function_index = lua_gettop(L);
	lua_pushtraceback(L);
	lua_insert(L, function_index);
	setup_std_vars(L, function_index);
	const int result = lua_pcall(L, 0, 1, function_index);
	const bool valid_result = result == 0 && lua_isboolean(L, -1);
	const bool allowed = !valid_result || lua_toboolean(L, -1) != 0;
	if(result != 0)
	{
		const char * message = lua_tostring(L, -1);
		log_message(L, "command filter failed: %s\n", message ? message : "non-string error");
	}
	else if(!valid_result)
		log_message(L, "command filter must return a boolean; command passed through\n");
	lua_pop(L, 1);
	lua_remove(L, function_index);
	return allowed;
}

static int XLuaFilterCommand(lua_State * L)
{
	xlua_cmd * d = luaL_checkuserdata<xlua_cmd>(L, 1, "expected command");
	xtlua_notify_cb_t * callback = wrap_lua_func(L, 2);
	xlua_cmd_install_filter(d, xlcmd_filter_helper, callback);
	return 0;
}
static int XTLuaReplaceCommand(lua_State * L)
{
	xtlua_cmd * d = luaL_checkuserdata<xtlua_cmd>(L,1,"expected command");
	xtlua_notify_cb_t * cb = wrap_lua_func(L, 2);
	
	xtlua_cmd_install_handler(d, cmd_cb_helper, cb);
	return 0;	
}
// XPLMWrapCommand cmd handler1 handler2
static int XTLuaWrapCommand(lua_State * L)
{
	xtlua_cmd * d = luaL_checkuserdata<xtlua_cmd>(L,1,"expected command");
	luaL_checktype(L, 2, LUA_TFUNCTION);
	luaL_checktype(L, 3, LUA_TFUNCTION);
	xtlua_notify_cb_t * cb1 = wrap_lua_func(L, 2);
	xtlua_notify_cb_t * cb2 = wrap_lua_func(L, 3);
	
	xtlua_cmd_install_pre_wrapper(d, cmd_cb_helper, cb1);
	xtlua_cmd_install_post_wrapper(d, cmd_cb_helper, cb2);
	return 0;	
}

// XPLMCommandStart cmd
static int XTLuaCommandStart(lua_State * L)
{
	xtlua_cmd * d = luaL_checkuserdata<xtlua_cmd>(L,1,"expected command");
	xtlua_cmd_start(d);
	return 0;
}

// XPLMCommandStop cmd
static int XTLuaCommandStop(lua_State * L)
{
	xtlua_cmd * d = luaL_checkuserdata<xtlua_cmd>(L,1,"expected command");
	xtlua_cmd_stop(d);
	return 0;
}

// XPLMCommandOnce cmd
static int XTLuaCommandOnce(lua_State * L)
{
	xtlua_cmd * d = luaL_checkuserdata<xtlua_cmd>(L,1,"expected command");
	xtlua_cmd_once(d);
	return 0;
}
static int XLuaCommandStart(lua_State * L)
{
	//printf("C++ command start");
	xlua_cmd * d = luaL_checkuserdata<xlua_cmd>(L,1,"expected command");
	if(xlua_cmd_is_dispatching(d))
		return luaL_error(L, "cannot recursively start the command currently being handled");
	xlua_cmd_start(d);
	return 0;
}

// XPLMCommandStop cmd
static int XLuaCommandStop(lua_State * L)
{
	xlua_cmd * d = luaL_checkuserdata<xlua_cmd>(L,1,"expected command");
	if(xlua_cmd_is_dispatching(d))
		return luaL_error(L, "cannot recursively stop the command currently being handled");
	xlua_cmd_stop(d);
	return 0;
}

// XPLMCommandOnce cmd
static int XLuaCommandOnce(lua_State * L)
{
	xlua_cmd * d = luaL_checkuserdata<xlua_cmd>(L,1,"expected command");
	if(xlua_cmd_is_dispatching(d))
		return luaL_error(L, "cannot recursively invoke the command currently being handled");
	xlua_cmd_once(d);
	return 0;
}
//----------------------------------------------------------------
// TIMERS
//----------------------------------------------------------------

static void timer_cb(void * ref)
{
	lua_State * L = setup_lua_callback(ref);
	if(L)
	{
		fmt_pcall_stdvars(L, 0, "");
	}	
}

// XPLMCreateTimer func -> ptr
static int XTLuaCreateTimer(lua_State * L)
{
	xtlua_notify_cb_t * helper = wrap_lua_func(L, 1);
	if(helper == NULL)
		return 0;
	
	xlua_timer * t = xtlua_create_timer(timer_cb, helper);
	if(t == NULL)
		return luaL_error(L, "unable to create timer");
	
	lua_pushlightuserdata(L, t);
	return 1;
}

// XPLMRunTimer timer delay repeat
static int XTLuaRunTimer(lua_State * L)
{
	xlua_timer * t = luaL_checkuserdata<xlua_timer>(L,1,"expected timer");
	if(!t)
		return 0;
	
	const double delay = luaL_check_finite_time(L, 2);
	const double repeat = luaL_check_finite_time(L, 3);
	xtlua_run_timer(t, delay, repeat);
	return 0;
}

// XPLMIsTimerScheduled ptr -> int
static int XTLuaIsTimerScheduled(lua_State * L)
{
	xlua_timer * t = luaL_checkuserdata<xlua_timer>(L,1,"expected timer");
	int sched = xtlua_is_timer_scheduled(t);
	lua_pushboolean(L, sched);
	return 1;
}
static int XLuaCreateTimer(lua_State * L)
{
	xtlua_notify_cb_t * helper = wrap_lua_func(L, 1);
	if(helper == NULL)
		return 0;
	
	xlua_timer * t = xlua_create_timer(timer_cb, helper);
	if(t == NULL)
		return luaL_error(L, "unable to create timer");
	
	lua_pushlightuserdata(L, t);
	return 1;
}

// XPLMRunTimer timer delay repeat
static int XLuaRunTimer(lua_State * L)
{
	xlua_timer * t = luaL_checkuserdata<xlua_timer>(L,1,"expected timer");
	if(!t)
		return 0;
	
	const double delay = luaL_check_finite_time(L, 2);
	const double repeat = luaL_check_finite_time(L, 3);
	xlua_run_timer(t, delay, repeat);
	return 0;
}

// XPLMIsTimerScheduled ptr -> int
static int XLuaIsTimerScheduled(lua_State * L)
{
	xlua_timer * t = luaL_checkuserdata<xlua_timer>(L,1,"expected timer");
	int sched = xlua_is_timer_scheduled(t);
	lua_pushboolean(L, sched);
	return 1;
}


static int XTLuaGetTimerRemaining(lua_State * L)
{
	xlua_timer * t = luaL_checkuserdata<xlua_timer>(L, 1, "expected timer");
	lua_pushnumber(L, xtlua_get_timer_remaining(t));
	return 1;
}

static int XLuaGetTimerRemaining(lua_State * L)
{
	xlua_timer * t = luaL_checkuserdata<xlua_timer>(L, 1, "expected timer");
	lua_pushnumber(L, xlua_get_timer_remaining(t));
	return 1;
}

//FUNC(XLuaCreateDataRef) 
#define XT_FUNC_LIST \
	FUNC(XLuaGetCode) \
	FUNC(XTLuaFindDataRef) \
	FUNC(XTLuaGetDataRefType) \
	FUNC(XTLuaGetNumber) \
	FUNC(XTLuaSetNumber) \
	FUNC(XTLuaGetArray) \
	FUNC(XTLuaSetArray) \
	FUNC(XTLuaGetArrayLength) \
	FUNC(XTLuaGetArrayValues) \
	FUNC(XTLuaSetArrayValues) \
	FUNC(XTLuaGetString) \
	FUNC(XTLuaSetString) \
	FUNC(XTLuaFindCommand) \
	FUNC(XTLuaReplaceCommand) \
	FUNC(XTLuaWrapCommand) \
	FUNC(XTLuaCommandStart) \
	FUNC(XTLuaCommandStop) \
	FUNC(XTLuaCommandOnce) \
	FUNC(XTLuaCreateTimer) \
	FUNC(XTLuaRunTimer) \
	FUNC(XTLuaGetTimerRemaining) \
	FUNC(XTLuaIsTimerScheduled)
#define XL_FUNC_LIST \
	FUNC(XLuaGetCode) \
	FUNC(XLuaFindDataRef) \
	FUNC(XLuaGetDataRefType) \
	FUNC(XLuaGetNumber) \
	FUNC(XLuaSetNumber) \
	FUNC(XLuaGetArray) \
	FUNC(XLuaSetArray) \
	FUNC(XLuaGetArrayLength) \
	FUNC(XLuaGetArrayValues) \
	FUNC(XLuaSetArrayValues) \
	FUNC(XLuaGetString) \
	FUNC(XLuaSetString) \
	FUNC(XLuaFindCommand) \
	FUNC(XLuaCreateCommand) \
	FUNC(XlLuaReplaceCommand) \
	FUNC(XLuaReplaceCommand) \
	FUNC(XLuaWrapCommand) \
	FUNC(XLuaFilterCommand) \
	FUNC(XLuaCommandStart) \
	FUNC(XLuaCommandStop) \
	FUNC(XLuaCommandOnce) \
	FUNC(XLuaCreateTimer) \
	FUNC(XLuaRunTimer) \
	FUNC(XLuaGetTimerRemaining) \
	FUNC(XLuaIsTimerScheduled) \
	FUNC(XLuaCreateDataRef)\
	FUNC(XLuaExistingDataRef)

static int classic_binding_dispatch(lua_State * L)
{
	module * owner = module::module_from_interp(L);
	if(owner == NULL)
		return luaL_error(L, "module context unavailable");
	if(owner->is_closing())
		return luaL_error(L, "XTLua module is closing; classic bindings are unavailable");

	// Keep a Lua C function value, not a function-pointer/object-pointer cast.
	// The gate precedes every original binding, including handle validation:
	// finalizers may still hold lightuserdata whose C++ object was retired.
	lua_CFunction function = lua_tocfunction(L, lua_upvalueindex(1));
	if(function == NULL)
		return luaL_error(L, "XTLua classic binding target unavailable");
	return function(L);
}

static void register_classic_binding(lua_State * L, const char * name, lua_CFunction function)
{
	lua_pushcfunction(L, function);
	lua_pushcclosure(L, classic_binding_dispatch, 1);
	lua_setglobal(L, name);
}

void	add_xpfuncs_to_interp(lua_State * L,bool isXT)
{
	#define FUNC(x) \
		register_classic_binding(L,#x,x);
	if(isXT){	
		XT_FUNC_LIST;
	}
	else{
		XL_FUNC_LIST;
	}
	#undef FUNC
}
