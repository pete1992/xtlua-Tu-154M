#ifndef XTLUA2_IMGUI_H
#define XTLUA2_IMGUI_H

extern "C" {
#include <lua.h>
}

// Only xlua2_main calls these functions. The worker never owns an ImGui
// context, window, texture, or XPLM callback.
void LoadImguiBindings(lua_State * L);
bool xtlua2_imgui_frame_active(lua_State * L);
void xtlua2_imgui_register_text_inputs(lua_State * L);
void xtlua2_imgui_cleanup_state(lua_State * L);

#endif
