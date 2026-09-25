#ifndef XTLUA_RENDER_BRIDGE_H
#define XTLUA_RENDER_BRIDGE_H

struct lua_State;
enum class module_runtime;

// Registers only the API appropriate to the owning Lua runtime. The worker
// publishes copied state; xlua2_main reads it from a main-thread draw callback.
void xtlua_register_render_bridge(lua_State * L, module_runtime runtime);

// Call after the worker has paused/joined, before module Lua states are closed.
void xtlua_clear_render_bridge();

#endif
