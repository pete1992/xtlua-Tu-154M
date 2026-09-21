//
//  xptimers.h
//  xlua
//
//  Created by Benjamin Supnik on 4/13/16.
//
//	Copyright 2016, Laminar Research
//	This source code is licensed under the MIT open source license.
//	See LICENSE.txt for the full terms of the license.

#ifndef xptimers_h
#define xptimers_h

extern "C" {
#include <lua.h>
}

typedef void (* xlua_timer_f)(void * ref);
struct xlua_timer;
struct xlua2_timer;

// Legacy XTLua/XLua 1 timers. These APIs and their two original timer lists are
// intentionally kept separate from the per-interpreter XLua 2 timers below.
xlua_timer *		xlua_create_timer(xlua_timer_f func, void * ref);
void				xlua_run_timer(xlua_timer * t, double delay, double repeat);
int				xlua_is_timer_scheduled(xlua_timer * t);
double			xlua_get_timer_remaining(xlua_timer * t);
xlua_timer *		xtlua_create_timer(xlua_timer_f func, void * ref);
void				xtlua_run_timer(xlua_timer * t, double delay, double repeat);
int				xtlua_is_timer_scheduled(xlua_timer * t);
double			xtlua_get_timer_remaining(xlua_timer * t);
void xlua_do_timers_for_time(double now,bool isPaused);
void xtlua_do_timers_for_time(double now,bool isPaused);
void xtlua_timer_cleanup();
double xlua_get_simulated_time();

// XLua 2 timers. Every timer is owned by the lua_State that created it. Timer
// handles are valid only in that state and remain allocated (scheduled or not)
// until explicitly destroyed or the state is cleaned up. This avoids dangling
// handles after a one-shot timer fires.
xlua2_timer * xlua2_create_timer(lua_State * L, int function_index);
xlua2_timer * xlua2_find_timer(lua_State * L, int function_index);
bool xlua2_run_timer(
	lua_State * L,
	xlua2_timer * timer,
	double delay,
	double repeat);
bool xlua2_is_timer_scheduled(lua_State * L, xlua2_timer * timer);
double xlua2_get_timer_remaining(lua_State * L, xlua2_timer * timer);
void xlua2_do_timers_for_time(double now);
bool xlua2_destroy_timer(lua_State * L, xlua2_timer * timer);

// Must be called while L is still open. Dropping a timer releases Lua registry
// references held by its callback record.
void xlua_remove_timers_for_state(lua_State * L);
void xlua2_timer_cleanup();

#endif
