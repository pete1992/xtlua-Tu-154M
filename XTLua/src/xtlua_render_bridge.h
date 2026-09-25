#ifndef XTLUA_RENDER_BRIDGE_H
#define XTLUA_RENDER_BRIDGE_H

struct lua_State;
enum class module_runtime;

// Registers only the API appropriate to the owning Lua runtime. Each channel
// uses a fixed lock-free SPSC triple buffer: the worker publishes copied state
// and xlua2_main reads only its main-thread render slot.
void xtlua_register_render_bridge(lua_State * L, module_runtime runtime);

// Main-thread frame boundary. Promotes at most one latest frame per channel;
// all XLuaGetRenderBuffer calls then read the same render slots for this frame.
void xtlua_swap_render_buffers();

// Call after the worker has paused/joined, before module Lua states are closed.
void xtlua_clear_render_bridge();

#endif
