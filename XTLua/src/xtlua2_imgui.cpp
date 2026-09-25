// XLua 2-style ImGui windows hosted by XTLua's xlua2_main runtime. This is
// deliberately separate from the asynchronous xtlua_worker runtime: Lua draw
// callbacks, ImGui, and every XPLM call below run on the X-Plane main thread.
#include "xtlua2_imgui.h"

#include "module.h"
#include "shared_xpfuncs.h"
#include "lua_helpers.h"
#include <XPLMDefs.h>
#include <XPLMDisplay.h>
#include <XPLMPanelGraphics.h>
#include <XPLMProcessing.h>

#include <imgui.h>

#include <cfloat>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <memory>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" XPLMWindowID * Make_XPLMWindowID(lua_State * L,
	XPLMWindowID const& initial);

namespace {

static_assert(sizeof(ImDrawVert) == 5 * sizeof(float),
	"XPLMPanelGraphics requires ImDrawVert's five-float layout");
static_assert(sizeof(ImDrawIdx) == sizeof(uint16_t),
	"XPLMPanelGraphics requires 16-bit ImGui indices");

class imgui_context {
public:
	explicit imgui_context(int width, int height, bool upload_now)
	{
		ImGuiContext * previous = ImGui::GetCurrentContext();
		context = ImGui::CreateContext();
		ImGui::SetCurrentContext(context);
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.ConfigMacOSXBehaviors = false;
		io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset |
			ImGuiBackendFlags_RendererHasTextures;
		io.DisplaySize = ImVec2(static_cast<float>(width),
			static_cast<float>(height));
		io.DeltaTime = 1.0f / 60.0f;
		// Lua draw callbacks can fail after Begin()/Push*(). Let ImGui recover
		// those stacks at EndFrame without asserting inside X-Plane's callback.
		io.ConfigErrorRecoveryEnableAssert = false;
		io.Fonts->AddFontDefault();

		// Prewarm the atlas before X-Plane invokes any panel draw callback.
		// XPLMCreateTexture is not legal from inside that callback.
		ImGui::NewFrame();
		ImGui::Begin("##xtlua2_font_preload");
		ImGui::TextUnformatted(
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
			"0123456789 .,;:!?+-*/()[]{}");
		ImGui::End();
		ImGui::Render();
		if(upload_now)
			update_textures();
		ImGui::SetCurrentContext(previous);
	}

	~imgui_context()
	{
		ImGuiContext * previous = ImGui::GetCurrentContext();
		for(void * texture : textures)
			XPLMDestroyTexture(texture);
		textures.clear();
		ImGui::DestroyContext(context);
		if(previous != context)
			ImGui::SetCurrentContext(previous);
	}

	imgui_context(const imgui_context&) = delete;
	imgui_context& operator=(const imgui_context&) = delete;

	ImGuiContext * get() const { return context; }
	bool in_frame() const { return frame_active; }
	std::vector<char>& text_buffer() { return text_scratch; }

	// Called only at creation and from the registered main-thread flight loop,
	// never from the panel draw callback. Recreate on incremental atlas updates
	// because XPLMCreateTexture has no sub-rectangle update counterpart.
	void update_textures()
	{
		ImGui::SetCurrentContext(context);
		for(ImTextureData * data : ImGui::GetPlatformIO().Textures)
		{
			if(data->Status == ImTextureStatus_WantDestroy)
			{
				void * old = data->BackendUserData;
				if(old != nullptr)
				{
					textures.erase(old);
					XPLMDestroyTexture(old);
				}
				data->BackendUserData = nullptr;
				data->SetTexID(ImTextureID_Invalid);
				data->SetStatus(ImTextureStatus_Destroyed);
				continue;
			}
			if(data->Status != ImTextureStatus_WantCreate &&
				data->Status != ImTextureStatus_WantUpdates)
				continue;
			if(data->Pixels == nullptr || data->Width <= 0 || data->Height <= 0)
				continue;

			const unsigned char * pixels = data->Pixels;
			std::vector<unsigned char> rgba;
			if(data->Format == ImTextureFormat_Alpha8)
			{
				const size_t count = static_cast<size_t>(data->Width) *
					static_cast<size_t>(data->Height);
				rgba.resize(count * 4);
				for(size_t i = 0; i < count; ++i)
				{
					rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = 255;
					rgba[4 * i + 3] = pixels[i];
				}
				pixels = rgba.data();
			}
			else if(data->Format != ImTextureFormat_RGBA32)
			{
				continue;
			}

			void * replacement = XPLMCreateTexture(pixels,
				data->Width, data->Height);
			if(replacement == nullptr)
				continue;
			void * old = data->BackendUserData;
			textures.insert(replacement);
			data->BackendUserData = replacement;
			data->SetTexID(static_cast<ImTextureID>(
				reinterpret_cast<uintptr_t>(replacement)));
			data->SetStatus(ImTextureStatus_OK);
			if(old != nullptr)
			{
				textures.erase(old);
				XPLMDestroyTexture(old);
			}
		}
	}

	void begin_frame(XPLMWindowID window, int width, int height)
	{
		ImGui::SetCurrentContext(context);
		current_window = window;
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(static_cast<float>((std::max)(width, 1)),
			static_cast<float>((std::max)(height, 1)));
		io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
		const float now = XPLMGetElapsedTime();
		if(last_frame_time > 0.0f && now > last_frame_time)
			io.DeltaTime = (std::min)(0.1f,
				(std::max)(0.001f, now - last_frame_time));
		last_frame_time = now;
		int x = 0, y = 0;
		XPLMGetMouseLocationGlobal(&x, &y);
		float local_x = 0.0f, local_y = 0.0f;
		if(translate_mouse(window, x, y, local_x, local_y))
			io.AddMousePosEvent(local_x, local_y);
		else
			io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
		push_modifiers();
		ImGui::NewFrame();
		frame_active = true;
	}

	void end_frame()
	{
		ImGui::SetCurrentContext(context);
		ImGui::Render();
		frame_active = false;
		const bool wants_text = ImGui::GetIO().WantTextInput;
		if(current_window != nullptr)
		{
			const bool owns_focus = XPLMHasKeyboardFocus(current_window) != 0;
			// An unfocused context may retain its active InputText item. Do not
			// let its next draw steal keyboard focus back from a sibling window.
			if(wants_text && !owns_focus && focus_request_allowed)
			{
				XPLMTakeKeyboardFocus(current_window);
				focus_request_allowed = false;
			}
			else if(!wants_text && owns_focus)
				XPLMTakeKeyboardFocus(nullptr);
			if(!wants_text)
				focus_request_allowed = true;
		}
		ImDrawData * draw = ImGui::GetDrawData();
		if(draw == nullptr)
			return;
		for(int list_index = 0; list_index < draw->CmdListsCount;
			++list_index)
		{
			ImDrawList * list = draw->CmdLists[list_index];
			XPLMMesh_t mesh{};
			mesh.vertex_count = list->VtxBuffer.Size;
			mesh.vertices = reinterpret_cast<const float *>(list->VtxBuffer.Data);
			mesh.index_count = list->IdxBuffer.Size;
			mesh.indices = reinterpret_cast<const uint16_t *>(list->IdxBuffer.Data);
			// Reuse capacity across draw lists and frames. Only this window's
			// main-thread context owns the scratch storage.
			std::vector<XPLMDrawCall_t>& calls = draw_calls;
			calls.clear();
			calls.reserve(static_cast<size_t>(list->CmdBuffer.Size));
			for(int i = 0; i < list->CmdBuffer.Size; ++i)
			{
				const ImDrawCmd& cmd = list->CmdBuffer[i];
				if(cmd.UserCallback != nullptr)
				{
					if(!calls.empty())
					{
						XPLMDrawCalls(&mesh, static_cast<int>(calls.size()),
							calls.data());
						calls.clear();
					}
					if(cmd.UserCallback != ImDrawCallback_ResetRenderState)
						cmd.UserCallback(list, &cmd);
					continue;
				}
				if(cmd.ElemCount == 0)
					continue;
				// A newly requested glyph texture is uploaded by the next main
				// flight-loop tick, outside this draw. Until then skip only its
				// draw call; never create a texture from a draw callback.
				const ImTextureID texture = cmd.TexRef._TexData != nullptr ?
					cmd.TexRef._TexData->TexID : cmd.TexRef._TexID;
				if(texture == ImTextureID_Invalid)
					continue;
				XPLMDrawCall_t call{};
				call.tex_ref = reinterpret_cast<void *>(
					static_cast<uintptr_t>(texture));
				call.scissors[0] = cmd.ClipRect.x;
				call.scissors[1] = cmd.ClipRect.y;
				call.scissors[2] = cmd.ClipRect.z;
				call.scissors[3] = cmd.ClipRect.w;
				call.idx_offset = static_cast<int>(cmd.IdxOffset);
				call.element_count = static_cast<int>(cmd.ElemCount);
				call.vtx_offset = static_cast<int>(cmd.VtxOffset);
				calls.push_back(call);
			}
			if(!calls.empty())
				XPLMDrawCalls(&mesh, static_cast<int>(calls.size()), calls.data());
		}
	}

	int mouse_button(XPLMWindowID win, int x, int y,
		XPLMMouseStatus status, int button)
	{
		ImGui::SetCurrentContext(context);
		push_modifiers();
		float local_x = 0.0f, local_y = 0.0f;
		if(translate_mouse(win, x, y, local_x, local_y))
			ImGui::GetIO().AddMousePosEvent(local_x, local_y);
		if(status == xplm_MouseDown || status == xplm_MouseUp)
		{
			if(status == xplm_MouseDown)
			{
				focus_request_allowed = true;
				ImGui::GetIO().AddFocusEvent(true);
			}
			ImGui::GetIO().AddMouseButtonEvent(button, status == xplm_MouseDown);
		}
		return ImGui::GetIO().WantCaptureMouse ? 1 : 0;
	}

	int mouse_wheel(XPLMWindowID win, int x, int y, int wheel,
		int clicks)
	{
		ImGui::SetCurrentContext(context);
		push_modifiers();
		float local_x = 0.0f, local_y = 0.0f;
		if(translate_mouse(win, x, y, local_x, local_y))
			ImGui::GetIO().AddMousePosEvent(local_x, local_y);
		ImGui::GetIO().AddMouseWheelEvent(
			wheel == 1 ? static_cast<float>(clicks) : 0.0f,
			wheel == 0 ? static_cast<float>(clicks) : 0.0f);
		return ImGui::GetIO().WantCaptureMouse ? 1 : 0;
	}

	void key(char key_char, XPLMKeyFlags flags, char virtual_key,
		int losing_focus)
	{
		ImGui::SetCurrentContext(context);
		ImGuiIO& io = ImGui::GetIO();
		if(losing_focus)
		{
			io.ClearInputKeys();
			io.AddFocusEvent(false);
			focus_request_allowed = false;
			return;
		}
		io.AddFocusEvent(true);
		push_modifiers();
		const unsigned char vk = static_cast<unsigned char>(virtual_key);
		ImGuiKey mapped = ImGuiKey_None;
		if(vk >= XPLM_VK_A && vk <= XPLM_VK_Z)
			mapped = static_cast<ImGuiKey>(ImGuiKey_A + vk - XPLM_VK_A);
		else if(vk >= XPLM_VK_0 && vk <= XPLM_VK_9)
			mapped = static_cast<ImGuiKey>(ImGuiKey_0 + vk - XPLM_VK_0);
		else if(vk >= XPLM_VK_F1 && vk <= XPLM_VK_F12)
			mapped = static_cast<ImGuiKey>(ImGuiKey_F1 + vk - XPLM_VK_F1);
		else
		{
			switch(vk)
			{
			case XPLM_VK_TAB: mapped = ImGuiKey_Tab; break;
			case XPLM_VK_LEFT: mapped = ImGuiKey_LeftArrow; break;
			case XPLM_VK_RIGHT: mapped = ImGuiKey_RightArrow; break;
			case XPLM_VK_UP: mapped = ImGuiKey_UpArrow; break;
			case XPLM_VK_DOWN: mapped = ImGuiKey_DownArrow; break;
			case XPLM_VK_PRIOR: mapped = ImGuiKey_PageUp; break;
			case XPLM_VK_NEXT: mapped = ImGuiKey_PageDown; break;
			case XPLM_VK_INSERT: mapped = ImGuiKey_Insert; break;
			case XPLM_VK_HOME: mapped = ImGuiKey_Home; break;
			case XPLM_VK_END: mapped = ImGuiKey_End; break;
			case XPLM_VK_BACK: mapped = ImGuiKey_Backspace; break;
			case XPLM_VK_DELETE: mapped = ImGuiKey_Delete; break;
			case XPLM_VK_SPACE: mapped = ImGuiKey_Space; break;
			case XPLM_VK_RETURN: mapped = ImGuiKey_Enter; break;
			case XPLM_VK_ENTER: mapped = ImGuiKey_KeypadEnter; break;
			case XPLM_VK_NUMPAD_ENT: mapped = ImGuiKey_KeypadEnter; break;
			case XPLM_VK_ESCAPE: mapped = ImGuiKey_Escape; break;
			default: break;
			}
		}
		if(mapped != ImGuiKey_None)
		{
			if((flags & xplm_DownFlag) != 0)
				io.AddKeyEvent(mapped, true);
			if((flags & xplm_UpFlag) != 0)
				io.AddKeyEvent(mapped, false);
		}
		if((flags & xplm_DownFlag) != 0 &&
			static_cast<unsigned char>(key_char) >= 32)
			io.AddInputCharacter(static_cast<unsigned char>(key_char));
	}

private:
	static bool translate_mouse(XPLMWindowID win, int x, int y,
		float& out_x, float& out_y)
	{
		int left = 0, top = 0, right = 0, bottom = 0;
		XPLMGetWindowGeometry(win, &left, &top, &right, &bottom);
		if(x < left || x > right || y < bottom || y > top)
			return false;
		out_x = static_cast<float>(x - left);
		out_y = static_cast<float>(top - y);
		return true;
	}

	static void push_modifiers()
	{
		const XPLMKeyFlags flags = XPLMGetModifierKeys();
		ImGuiIO& io = ImGui::GetIO();
		io.AddKeyEvent(ImGuiMod_Shift, (flags & xplm_ShiftFlag) != 0);
		io.AddKeyEvent(ImGuiMod_Ctrl, (flags & xplm_ControlFlag) != 0);
		io.AddKeyEvent(ImGuiMod_Alt, (flags & xplm_OptionAltFlag) != 0);
	}

	ImGuiContext * context = nullptr;
	XPLMWindowID current_window = nullptr;
	bool frame_active = false;
	bool focus_request_allowed = true;
	float last_frame_time = 0.0f;
	std::unordered_set<void *> textures;
	std::vector<XPLMDrawCall_t> draw_calls;
	std::vector<char> text_scratch;
};

struct window_state {
	lua_State * L = nullptr;
	std::unique_ptr<imgui_context> imgui;
	int draw_ref = LUA_NOREF;
	bool drawing = false;
	bool pending_destroy = false;
};

std::unordered_map<XPLMWindowID, std::unique_ptr<window_state>> windows;
// The active context is selected by the native window callback, never merely
// by its Lua state: one xlua2_main state may own several independent windows.
lua_State * drawing_lua = nullptr;
imgui_context * drawing_context = nullptr;
bool texture_loop_registered = false;
bool texture_loop_running = false;
float texture_tick(float, float, int, void *);

imgui_context * active_context(lua_State * L)
{
	return drawing_lua == L && drawing_context != nullptr &&
		drawing_context->in_frame() &&
		ImGui::GetCurrentContext() == drawing_context->get() ?
		drawing_context : nullptr;
}

void destroy_window(XPLMWindowID win)
{
	auto found = windows.find(win);
	if(found == windows.end())
		return;
	window_state * state = found->second.get();
	if(drawing_context != nullptr || state->drawing)
	{
		state->pending_destroy = true;
		return;
	}
	// Detach before SDK destruction: a native callback can reenter and attempt
	// to destroy the same handle. Retain the state until that SDK call returns.
	state->pending_destroy = true;
	std::unique_ptr<window_state> retired = std::move(found->second);
	windows.erase(found);
	XPLMDestroyWindow(win);
	if(state->draw_ref != LUA_NOREF)
		luaL_unref(state->L, LUA_REGISTRYINDEX, state->draw_ref);
	if(windows.empty() && texture_loop_registered && !texture_loop_running)
	{
		XPLMUnregisterFlightLoopCallback(texture_tick, nullptr);
		texture_loop_registered = false;
	}
}

float texture_tick(float, float, int, void *)
{
	if(drawing_context != nullptr)
		return -1.0f;
	texture_loop_running = true;
	// Destroying textures/windows is deferred until the draw callback has
	// returned, including a Lua request to destroy a different window.
	for(auto it = windows.begin(); it != windows.end(); )
	{
		const XPLMWindowID id = it->first;
		const bool pending = it->second->pending_destroy;
		++it;
		if(pending)
			destroy_window(id);
	}
	ImGuiContext * previous = ImGui::GetCurrentContext();
	for(auto& entry : windows)
		entry.second->imgui->update_textures();
	ImGui::SetCurrentContext(previous);
	texture_loop_running = false;
	// The SDK forbids unregistering this callback from its own stack. Returning
	// zero leaves an inactive registration; the next window reactivates it.
	return windows.empty() ? 0.0f : -1.0f;
}

void draw_window(XPLMWindowID win, void * refcon)
{
	window_state * state = static_cast<window_state *>(refcon);
	if(state == nullptr || state->pending_destroy || drawing_context != nullptr)
		return;
	module * owner = module::module_from_interp(state->L);
	if(owner == nullptr || owner->is_closing() || !owner->is_enabled())
		return;
	int left = 0, top = 0, right = 0, bottom = 0;
	XPLMGetWindowGeometry(win, &left, &top, &right, &bottom);
	ImGuiContext * previous = ImGui::GetCurrentContext();
	state->drawing = true;
	drawing_lua = state->L;
	drawing_context = state->imgui.get();
	state->imgui->begin_frame(win, right - left, top - bottom);
	if(state->draw_ref != LUA_NOREF)
	{
		const int debug = lua_pushtraceback(state->L);
		lua_rawgeti(state->L, LUA_REGISTRYINDEX, state->draw_ref);
		Make_XPLMWindowID(state->L, win);
		lua_pushinteger(state->L, right - left);
		lua_pushinteger(state->L, top - bottom);
		if(lua_pcall(state->L, 3, 0, debug) != 0)
		{
			const char * message = lua_tostring(state->L, -1);
			log_message(state->L, "ImGui draw callback failed: %s\n",
				message ? message : "(non-string Lua error)");
			lua_pop(state->L, 1);
		}
		lua_remove(state->L, debug);
	}
	state->imgui->end_frame();
	ImGui::SetCurrentContext(previous);
	state->drawing = false;
	drawing_context = nullptr;
	drawing_lua = nullptr;
}

int mouse_click(XPLMWindowID win, int x, int y,
	XPLMMouseStatus status, void * refcon)
{
	window_state * state = static_cast<window_state *>(refcon);
	if(state == nullptr || state->pending_destroy)
		return 0;
	ImGuiContext * previous = ImGui::GetCurrentContext();
	const int result = state->imgui->mouse_button(win, x, y, status, 0);
	ImGui::SetCurrentContext(previous);
	return result;
}

int right_click(XPLMWindowID win, int x, int y,
	XPLMMouseStatus status, void * refcon)
{
	window_state * state = static_cast<window_state *>(refcon);
	if(state == nullptr || state->pending_destroy)
		return 0;
	ImGuiContext * previous = ImGui::GetCurrentContext();
	const int result = state->imgui->mouse_button(win, x, y, status, 1);
	ImGui::SetCurrentContext(previous);
	return result;
}

void key_event(XPLMWindowID, char key, XPLMKeyFlags flags,
	char virtual_key, void * refcon, int losing_focus)
{
	window_state * state = static_cast<window_state *>(refcon);
	if(state == nullptr || state->pending_destroy)
		return;
	ImGuiContext * previous = ImGui::GetCurrentContext();
	state->imgui->key(key, flags, virtual_key, losing_focus);
	ImGui::SetCurrentContext(previous);
}

XPLMCursorStatus cursor_event(XPLMWindowID, int, int, void *)
{
	return xplm_CursorDefault;
}

int wheel_event(XPLMWindowID win, int x, int y, int wheel,
	int clicks, void * refcon)
{
	window_state * state = static_cast<window_state *>(refcon);
	if(state == nullptr || state->pending_destroy)
		return 0;
	ImGuiContext * previous = ImGui::GetCurrentContext();
	const int result = state->imgui->mouse_wheel(win, x, y, wheel, clicks);
	ImGui::SetCurrentContext(previous);
	return result;
}

int table_int(lua_State * L, const char * key, int fallback)
{
	lua_getfield(L, 1, key);
	const lua_Integer value = lua_isnil(L, -1) ? fallback :
		luaL_checkinteger(L, -1);
	if(value < (std::numeric_limits<int>::min)() ||
		value > (std::numeric_limits<int>::max)())
		luaL_error(L, "window field '%s' exceeds integer range", key);
	lua_pop(L, 1);
	return static_cast<int>(value);
}

bool table_bool(lua_State * L, const char * key, bool fallback)
{
	lua_getfield(L, 1, key);
	const bool value = lua_isnil(L, -1) ? fallback :
		lua_toboolean(L, -1) != 0;
	lua_pop(L, 1);
	return value;
}

int text_buffer_capacity(lua_State * L, int argument, int fallback, size_t length)
{
	const lua_Integer requested = luaL_optinteger(L, argument, fallback);
	if(requested > 1024 * 1024 || length >= 1024 * 1024)
		return luaL_argerror(L, argument, "text buffer exceeds 1 MiB");
	const int capacity = requested < 1 ? 1 : static_cast<int>(requested);
	return (std::max)(capacity, static_cast<int>(length) + 1);
}

// The vendored macro-generated binder cannot represent char* + capacity
// input widgets. Keep the normal Lua convention: (changed, updated_text).
int input_text(lua_State * L)
{
	const char * label = luaL_checkstring(L, 1);
	size_t length = 0;
	const char * original = luaL_checklstring(L, 2, &length);
	const int capacity = text_buffer_capacity(L, 3, 256, length);
	const int flags = static_cast<int>(luaL_optinteger(L, 4, 0));
	if((flags & (ImGuiInputTextFlags_CallbackCompletion |
		ImGuiInputTextFlags_CallbackHistory |
		ImGuiInputTextFlags_CallbackAlways |
		ImGuiInputTextFlags_CallbackCharFilter |
		ImGuiInputTextFlags_CallbackResize |
		ImGuiInputTextFlags_CallbackEdit)) != 0)
		return luaL_argerror(L, 4, "callback flags are not supported");
	imgui_context * context = active_context(L);
	if(context == nullptr)
	{
		lua_pushboolean(L, 0);
		lua_pushvalue(L, 2);
		return 2;
	}
	std::vector<char>& buffer = context->text_buffer();
	buffer.resize(static_cast<size_t>(capacity));
	std::memcpy(buffer.data(), original, length);
	buffer[length] = '\0';
	const bool changed = ImGui::InputText(label, buffer.data(),
		buffer.size(), static_cast<ImGuiInputTextFlags>(flags));
	lua_pushboolean(L, changed);
	lua_pushstring(L, buffer.data());
	return 2;
}

int input_text_with_hint(lua_State * L)
{
	const char * label = luaL_checkstring(L, 1);
	const char * hint = luaL_checkstring(L, 2);
	size_t length = 0;
	const char * original = luaL_checklstring(L, 3, &length);
	const int capacity = text_buffer_capacity(L, 4, 256, length);
	const int flags = static_cast<int>(luaL_optinteger(L, 5, 0));
	if((flags & (ImGuiInputTextFlags_CallbackCompletion |
		ImGuiInputTextFlags_CallbackHistory |
		ImGuiInputTextFlags_CallbackAlways |
		ImGuiInputTextFlags_CallbackCharFilter |
		ImGuiInputTextFlags_CallbackResize |
		ImGuiInputTextFlags_CallbackEdit)) != 0)
		return luaL_argerror(L, 5, "callback flags are not supported");
	imgui_context * context = active_context(L);
	if(context == nullptr)
	{
		lua_pushboolean(L, 0);
		lua_pushvalue(L, 3);
		return 2;
	}
	std::vector<char>& buffer = context->text_buffer();
	buffer.resize(static_cast<size_t>(capacity));
	std::memcpy(buffer.data(), original, length);
	buffer[length] = '\0';
	const bool changed = ImGui::InputTextWithHint(label, hint, buffer.data(),
		buffer.size(), static_cast<ImGuiInputTextFlags>(flags));
	lua_pushboolean(L, changed);
	lua_pushstring(L, buffer.data());
	return 2;
}

int input_text_multiline(lua_State * L)
{
	const char * label = luaL_checkstring(L, 1);
	size_t length = 0;
	const char * original = luaL_checklstring(L, 2, &length);
	const int capacity = text_buffer_capacity(L, 3, 4096, length);
	const float width = static_cast<float>(luaL_optnumber(L, 4, 0.0));
	const float height = static_cast<float>(luaL_optnumber(L, 5, 0.0));
	const int flags = static_cast<int>(luaL_optinteger(L, 6, 0));
	if((flags & (ImGuiInputTextFlags_CallbackCompletion |
		ImGuiInputTextFlags_CallbackHistory |
		ImGuiInputTextFlags_CallbackAlways |
		ImGuiInputTextFlags_CallbackCharFilter |
		ImGuiInputTextFlags_CallbackResize |
		ImGuiInputTextFlags_CallbackEdit)) != 0)
		return luaL_argerror(L, 6, "callback flags are not supported");
	imgui_context * context = active_context(L);
	if(context == nullptr)
	{
		lua_pushboolean(L, 0);
		lua_pushvalue(L, 2);
		return 2;
	}
	std::vector<char>& buffer = context->text_buffer();
	buffer.resize(static_cast<size_t>(capacity));
	std::memcpy(buffer.data(), original, length);
	buffer[length] = '\0';
	const bool changed = ImGui::InputTextMultiline(label, buffer.data(),
		buffer.size(), ImVec2(width, height),
		static_cast<ImGuiInputTextFlags>(flags));
	lua_pushboolean(L, changed);
	lua_pushstring(L, buffer.data());
	return 2;
}

} // namespace

bool xtlua2_imgui_frame_active(lua_State * L)
{
	return active_context(L) != nullptr;
}

void xtlua2_imgui_register_text_inputs(lua_State * L)
{
	lua_getglobal(L, "imgui");
	if(lua_istable(L, -1))
	{
		lua_pushcfunction(L, input_text);
		lua_setfield(L, -2, "InputText");
		lua_pushcfunction(L, input_text_with_hint);
		lua_setfield(L, -2, "InputTextWithHint");
		lua_pushcfunction(L, input_text_multiline);
		lua_setfield(L, -2, "InputTextMultiline");
	}
	lua_pop(L, 1);
}

extern "C" int XLuaCreateImguiWindow(lua_State * L)
{
	module * owner = module::module_from_interp(L);
	if(owner == nullptr || owner->get_runtime() != module_runtime::xlua2_main)
		return luaL_error(L, "XLuaCreateImguiWindow requires xlua2_main");
	luaL_checktype(L, 1, LUA_TTABLE);
	XPLMCreateWindow_t params{};
	params.structSize = sizeof(params);
	params.left = table_int(L, "left", 100);
	params.top = table_int(L, "top", 500);
	params.right = table_int(L, "right", 600);
	params.bottom = table_int(L, "bottom", 100);
	if(params.right <= params.left || params.top <= params.bottom)
		return luaL_error(L, "ImGui window geometry must have positive size");
	if(static_cast<int64_t>(params.right) - params.left >
		(std::numeric_limits<int>::max)() ||
		static_cast<int64_t>(params.top) - params.bottom >
		(std::numeric_limits<int>::max)())
		return luaL_error(L, "ImGui window dimensions exceed integer range");
	params.visible = table_bool(L, "visible", true) ? 1 : 0;
	params.decorateAsFloatingWindow = static_cast<XPLMWindowDecoration>(
		table_int(L, "decorateAsFloatingWindow",
			xplm_WindowDecorationRoundRectangle));
	params.layer = static_cast<XPLMWindowLayer>(
		table_int(L, "layer", xplm_WindowLayerFloatingWindows));
	params.contentType = xplm_WindowContentTypePanelGraphics;

	lua_getfield(L, 1, "drawWindowFunc");
	if(!lua_isnil(L, -1) && !lua_isfunction(L, -1))
		return luaL_error(L, "drawWindowFunc must be a function");
	std::unique_ptr<window_state> window(new window_state());
	window->L = L;
	if(!lua_isnil(L, -1))
	{
		window->draw_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	else
	{
		lua_pop(L, 1);
	}

	// Each native window owns all ImGui state (input/focus, font atlas, timing,
	// scratch buffers). A window created from a draw callback uploads textures
	// on the next main flight-loop tick, not recursively from that callback.
	window->imgui.reset(new imgui_context(params.right - params.left,
		params.top - params.bottom, drawing_context == nullptr));
	params.refcon = window.get();
	params.drawWindowFunc = draw_window;
	params.handleMouseClickFunc = mouse_click;
	params.handleRightClickFunc = right_click;
	params.handleKeyFunc = key_event;
	params.handleCursorFunc = cursor_event;
	params.handleMouseWheelFunc = wheel_event;

	XPLMWindowID id = XPLMCreateWindowEx(&params);
	if(id == nullptr)
	{
		if(window->draw_ref != LUA_NOREF)
			luaL_unref(L, LUA_REGISTRYINDEX, window->draw_ref);
		lua_pushnil(L);
		return 1;
	}
	windows.emplace(id, std::move(window));
	if(!texture_loop_registered)
	{
		XPLMRegisterFlightLoopCallback(texture_tick, -1.0f, nullptr);
		texture_loop_registered = true;
	}
	else if(!texture_loop_running)
	{
		XPLMSetFlightLoopCallbackInterval(texture_tick, -1.0f, 1, nullptr);
	}
	Make_XPLMWindowID(L, id);
	return 1;
}

extern "C" int XLuaDestroyImguiWindow(lua_State * L)
{
	module * owner = module::module_from_interp(L);
	if(owner == nullptr || owner->get_runtime() != module_runtime::xlua2_main)
		return luaL_error(L, "XLuaDestroyImguiWindow requires xlua2_main");
	XPLMWindowID id = xlua_checkuserdata<XPLMWindowID>(
		L, 1, "Expected XPLMWindowID");
	auto found = windows.find(id);
	if(found == windows.end() || found->second->L != L)
		return luaL_error(L, "window is not owned by this xlua2_main state");
	destroy_window(id);
	return 0;
}

void xtlua2_imgui_cleanup_state(lua_State * L)
{
	for(auto it = windows.begin(); it != windows.end(); )
	{
		if(it->second->L != L)
		{
			++it;
			continue;
		}
		XPLMWindowID id = it->first;
		++it;
		destroy_window(id);
	}
	if(windows.empty() && texture_loop_registered && !texture_loop_running)
	{
		XPLMUnregisterFlightLoopCallback(texture_tick, nullptr);
		texture_loop_registered = false;
	}
}
