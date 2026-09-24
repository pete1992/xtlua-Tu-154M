#ifndef XTLUA_XLUA2_HOST_H
#define XTLUA_XLUA2_HOST_H

#include <string>

extern "C" {
#include <lua.h>

// Hand-written implementations for SDK declarations marked
// lua_impl="external". Keep these names and C linkage in sync with the
// generated XLua_Register_glue.cpp declarations.
int XLuaCreateTimer(lua_State * L);
int XLuaRunTimer(lua_State * L);
int XLuaFindTimer(lua_State * L);
int XLuaIsTimerScheduled(lua_State * L);
int XLuaGetTimerRemaining(lua_State * L);
int XLuaReloadOnFlightChange(lua_State * L);
int XLuaCreateImguiWindow(lua_State * L);
int XLuaDestroyImguiWindow(lua_State * L);
}

// Implemented by the owning plugin host (xlua.cpp). Keeping the binding free
// of plugin globals also lets the same generated XLua API be hosted elsewhere.
void xlua2_host_reload_on_flight_change();
std::string xlua2_event_param_type(int event_id);

// Host lifecycle hook: release every timer callback owned by L before the
// interpreter is closed.
void xlua_remove_timers_for_state(lua_State * L);

#endif
