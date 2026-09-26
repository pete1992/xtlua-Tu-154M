#include <stdio.h>
#include <imgui.h>
#include <deque>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <limits>
#include "../xtlua2_imgui.h"
#include "../shared_xpfuncs.h"

extern "C" {
  #include "lua.h"
  #include "lualib.h"
  #include "lauxlib.h"
}

namespace {

// Explicit adapters for public ImGui 1.92 signatures which the historical
// generator skips (new flag typedefs, ImVec2 defaults and integer returns).
// They do not expose context/frame lifetime, native pointers or callbacks.
void require_draw_frame(lua_State* L)
{
  if (!xtlua2_imgui_frame_active(L))
    luaL_error(L, "imgui functions require an active xlua2_main draw callback");
}

float opt_float(lua_State* L, int index, float fallback)
{
  const lua_Number value = luaL_optnumber(L, index, fallback);
  if (!std::isfinite(value) || value < -FLT_MAX || value > FLT_MAX)
    luaL_argerror(L, index, "expected finite float-range number");
  return static_cast<float>(value);
}

float check_float(lua_State* L, int index)
{
  luaL_checknumber(L, index);
  return opt_float(L, index, 0);
}

int opt_int(lua_State* L, int index, int fallback = 0)
{
  const lua_Integer value = luaL_optinteger(L, index, fallback);
  if (value < (std::numeric_limits<int>::min)() ||
      value > (std::numeric_limits<int>::max)())
    luaL_argerror(L, index, "expected int-range value");
  return static_cast<int>(value);
}

int check_int(lua_State* L, int index)
{
  luaL_checkinteger(L, index);
  return opt_int(L, index);
}

const char* numeric_format(lua_State* L, int index, const char* fallback,
  bool floating_point)
{
  size_t length = 0;
  const char* format = luaL_optlstring(L, index, fallback, &length);
  if (length > 256 || std::memchr(format, '\0', length) != nullptr)
    luaL_argerror(L, index, "numeric format must be at most 256 bytes without NUL");
  unsigned conversions = 0;
  for (size_t cursor = 0; cursor < length; )
  {
    if (format[cursor++] != '%')
      continue;
    if (cursor < length && format[cursor] == '%')
    {
      ++cursor;
      continue;
    }
    if (++conversions != 1)
      luaL_argerror(L, index, "numeric format requires one conversion");
    while (cursor < length && std::strchr("-+ #0", format[cursor]) != nullptr)
      ++cursor;
    unsigned width = 0;
    while (cursor < length && format[cursor] >= '0' && format[cursor] <= '9')
    {
      width = width * 10 + static_cast<unsigned>(format[cursor++] - '0');
      if (width > 128)
        luaL_argerror(L, index, "numeric format width exceeds 128");
    }
    if (cursor < length && format[cursor] == '.')
    {
      ++cursor;
      unsigned precision = 0;
      while (cursor < length && format[cursor] >= '0' && format[cursor] <= '9')
      {
        precision = precision * 10 + static_cast<unsigned>(format[cursor++] - '0');
        if (precision > 32)
          luaL_argerror(L, index, "numeric format precision exceeds 32");
      }
    }
    // Reject *, positional arguments, length modifiers and pointer/string/%n
    // conversions: ImGui supplies exactly one promoted float or 32-bit integer.
    const char* allowed = floating_point ? "aAeEfFgG" : "diouxX";
    if (cursor == length || std::strchr(allowed, format[cursor]) == nullptr)
      luaL_argerror(L, index, "numeric format has an incompatible conversion");
    ++cursor;
  }
  if (conversions != 1)
    luaL_argerror(L, index, "numeric format requires one conversion");
  return format;
}

void check_float_slider_range(lua_State* L, float minimum, float maximum)
{
  if (minimum > maximum)
    luaL_argerror(L, 3, "slider minimum must not exceed maximum");
  if (minimum < -FLT_MAX / 2.0f || maximum > FLT_MAX / 2.0f)
    luaL_argerror(L, 3, "slider bounds exceed ImGui's float half-range");
}

void check_int_slider_range(lua_State* L, int minimum, int maximum)
{
  if (minimum > maximum)
    luaL_argerror(L, 3, "slider minimum must not exceed maximum");
  if (minimum < (std::numeric_limits<int>::min)() / 2 ||
      maximum > (std::numeric_limits<int>::max)() / 2)
    luaL_argerror(L, 3, "slider bounds exceed ImGui's integer half-range");
}

int numeric_input_flags(lua_State* L, int index)
{
  const int flags = opt_int(L, index);
  if ((flags & (ImGuiInputTextFlags_CallbackCompletion |
      ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackAlways |
      ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_CallbackResize |
      ImGuiInputTextFlags_CallbackEdit)) != 0)
    luaL_argerror(L, index, "callback flags are not supported for numeric input");
  return flags;
}

int push_bool(lua_State* L, bool value)
{
  lua_pushboolean(L, value);
  return 1;
}

int extra_BeginChild(lua_State* L)
{
  require_draw_frame(L);
  const char* id = luaL_checkstring(L, 1);
  return push_bool(L, ImGui::BeginChild(id,
    ImVec2(opt_float(L, 2, 0), opt_float(L, 3, 0)),
    opt_int(L, 4), opt_int(L, 5)));
}

int extra_SetNextWindowPos(lua_State* L)
{
  require_draw_frame(L);
  ImGui::SetNextWindowPos(ImVec2(check_float(L, 1),
    check_float(L, 2)), opt_int(L, 3),
    ImVec2(opt_float(L, 4, 0), opt_float(L, 5, 0)));
  return 0;
}

int extra_SetNextWindowSize(lua_State* L)
{
  require_draw_frame(L);
  ImGui::SetNextWindowSize(ImVec2(check_float(L, 1),
    check_float(L, 2)), opt_int(L, 3));
  return 0;
}

int extra_SetNextWindowCollapsed(lua_State* L)
{
  require_draw_frame(L);
  ImGui::SetNextWindowCollapsed(lua_toboolean(L, 1) != 0, opt_int(L, 2));
  return 0;
}

int extra_BeginCombo(lua_State* L)
{
  require_draw_frame(L);
  return push_bool(L, ImGui::BeginCombo(luaL_checkstring(L, 1),
    luaL_optstring(L, 2, ""), opt_int(L, 3)));
}

int extra_BeginListBox(lua_State* L)
{
  require_draw_frame(L);
  return push_bool(L, ImGui::BeginListBox(luaL_checkstring(L, 1),
    ImVec2(opt_float(L, 2, 0), opt_float(L, 3, 0))));
}

int extra_Selectable(lua_State* L)
{
  require_draw_frame(L);
  return push_bool(L, ImGui::Selectable(luaL_checkstring(L, 1),
    lua_toboolean(L, 2) != 0, opt_int(L, 3),
    ImVec2(opt_float(L, 4, 0), opt_float(L, 5, 0))));
}

int extra_BeginTable(lua_State* L)
{
  require_draw_frame(L);
  const char* id = luaL_checkstring(L, 1);
  const int columns = check_int(L, 2);
  if (columns < 1 || columns >= 512)
    return luaL_argerror(L, 2, "table must have 1..511 columns");
  return push_bool(L, ImGui::BeginTable(id, columns, opt_int(L, 3),
    ImVec2(opt_float(L, 4, 0), opt_float(L, 5, 0)), opt_float(L, 6, 0)));
}

int extra_TableNextRow(lua_State* L)
{
  require_draw_frame(L);
  ImGui::TableNextRow(opt_int(L, 1), opt_float(L, 2, 0));
  return 0;
}

int extra_TableSetupColumn(lua_State* L)
{
  require_draw_frame(L);
  ImGui::TableSetupColumn(luaL_checkstring(L, 1), opt_int(L, 2),
    opt_float(L, 3, 0), static_cast<ImGuiID>(luaL_optinteger(L, 4, 0)));
  return 0;
}

int extra_BeginTabBar(lua_State* L)
{
  require_draw_frame(L);
  return push_bool(L, ImGui::BeginTabBar(luaL_checkstring(L, 1), opt_int(L, 2)));
}

int extra_BeginTabItem(lua_State* L)
{
  require_draw_frame(L);
  const char* label = luaL_checkstring(L, 1);
  const bool has_open = !lua_isnoneornil(L, 2);
  bool open = lua_toboolean(L, 2) != 0;
  const bool selected = ImGui::BeginTabItem(label,
    has_open ? &open : nullptr, opt_int(L, 3));
  lua_pushboolean(L, selected);
  if (has_open)
    lua_pushboolean(L, open);
  return has_open ? 2 : 1;
}

int extra_TreeNodeEx(lua_State* L)
{
  require_draw_frame(L);
  return push_bool(L, ImGui::TreeNodeEx(luaL_checkstring(L, 1), opt_int(L, 2)));
}

int extra_CollapsingHeader(lua_State* L)
{
  require_draw_frame(L);
  return push_bool(L, ImGui::CollapsingHeader(luaL_checkstring(L, 1), opt_int(L, 2)));
}

int extra_SetNextItemOpen(lua_State* L)
{
  require_draw_frame(L);
  ImGui::SetNextItemOpen(lua_toboolean(L, 1) != 0, opt_int(L, 2));
  return 0;
}

int extra_DragFloat(lua_State* L)
{
  require_draw_frame(L);
  const char* label = luaL_checkstring(L, 1);
  float value = check_float(L, 2);
  const char* format = numeric_format(L, 6, "%.3f", true);
  const bool changed = ImGui::DragFloat(label, &value, opt_float(L, 3, 1),
    opt_float(L, 4, 0), opt_float(L, 5, 0), format,
    opt_int(L, 7));
  lua_pushboolean(L, changed);
  lua_pushnumber(L, value);
  return 2;
}

int extra_DragInt(lua_State* L)
{
  require_draw_frame(L);
  const char* label = luaL_checkstring(L, 1);
  int value = check_int(L, 2);
  const char* format = numeric_format(L, 6, "%d", false);
  const bool changed = ImGui::DragInt(label, &value, opt_float(L, 3, 1),
    opt_int(L, 4), opt_int(L, 5), format, opt_int(L, 7));
  lua_pushboolean(L, changed);
  lua_pushinteger(L, value);
  return 2;
}

int extra_SliderFloat(lua_State* L)
{
  require_draw_frame(L);
  const char* label = luaL_checkstring(L, 1);
  float value = check_float(L, 2);
  const float minimum = check_float(L, 3);
  const float maximum = check_float(L, 4);
  check_float_slider_range(L, minimum, maximum);
  const char* format = numeric_format(L, 5, "%.3f", true);
  const bool changed = ImGui::SliderFloat(label, &value, minimum, maximum,
    format, opt_int(L, 6));
  lua_pushboolean(L, changed);
  lua_pushnumber(L, value);
  return 2;
}

int extra_SliderInt(lua_State* L)
{
  require_draw_frame(L);
  const char* label = luaL_checkstring(L, 1);
  int value = check_int(L, 2);
  const int minimum = check_int(L, 3);
  const int maximum = check_int(L, 4);
  check_int_slider_range(L, minimum, maximum);
  const char* format = numeric_format(L, 5, "%d", false);
  const bool changed = ImGui::SliderInt(label, &value, minimum, maximum,
    format, opt_int(L, 6));
  lua_pushboolean(L, changed);
  lua_pushinteger(L, value);
  return 2;
}

int extra_InputFloat(lua_State* L)
{
  require_draw_frame(L);
  const char* label = luaL_checkstring(L, 1);
  float value = check_float(L, 2);
  const float step = opt_float(L, 3, 0);
  const float fast_step = opt_float(L, 4, 0);
  const char* format = numeric_format(L, 5, "%.3f", true);
  const int flags = numeric_input_flags(L, 6);
  const bool changed = ImGui::InputFloat(label, &value, step, fast_step,
    format, flags);
  lua_pushboolean(L, changed);
  lua_pushnumber(L, value);
  return 2;
}

int extra_InputInt(lua_State* L)
{
  require_draw_frame(L);
  const char* label = luaL_checkstring(L, 1);
  int value = check_int(L, 2);
  const int step = opt_int(L, 3, 1);
  const int fast_step = opt_int(L, 4, 100);
  const int flags = numeric_input_flags(L, 5);
  const bool changed = ImGui::InputInt(label, &value, step, fast_step, flags);
  lua_pushboolean(L, changed);
  lua_pushinteger(L, value);
  return 2;
}

int extra_ProgressBar(lua_State* L)
{
  require_draw_frame(L);
  ImGui::ProgressBar(check_float(L, 1),
    ImVec2(opt_float(L, 2, -FLT_MIN), opt_float(L, 3, 0)),
    luaL_optstring(L, 4, nullptr));
  return 0;
}

int extra_InvisibleButton(lua_State* L)
{
  require_draw_frame(L);
  return push_bool(L, ImGui::InvisibleButton(luaL_checkstring(L, 1),
    ImVec2(check_float(L, 2), check_float(L, 3)), opt_int(L, 4)));
}

#define XTLUA_IMGUI_FLAG_QUERY(name) \
int extra_##name(lua_State* L) { \
  require_draw_frame(L); \
  return push_bool(L, ImGui::name(opt_int(L, 1))); \
}
XTLUA_IMGUI_FLAG_QUERY(IsWindowFocused)
XTLUA_IMGUI_FLAG_QUERY(IsWindowHovered)
XTLUA_IMGUI_FLAG_QUERY(IsItemHovered)
#undef XTLUA_IMGUI_FLAG_QUERY

#define XTLUA_IMGUI_INTEGER_QUERY(name) \
int extra_##name(lua_State* L) { \
  require_draw_frame(L); \
  lua_pushinteger(L, ImGui::name()); \
  return 1; \
}
XTLUA_IMGUI_INTEGER_QUERY(TableGetColumnCount)
XTLUA_IMGUI_INTEGER_QUERY(TableGetColumnIndex)
XTLUA_IMGUI_INTEGER_QUERY(TableGetRowIndex)
#undef XTLUA_IMGUI_INTEGER_QUERY

// Lua strings are text, not printf formats. A percent sign in a display label
// must never request a missing C vararg. Use string.format() in Lua as needed.
#define XTLUA_IMGUI_TEXT(name) \
int extra_##name(lua_State* L) { \
  require_draw_frame(L); \
  ImGui::name("%s", luaL_checkstring(L, 1)); \
  return 0; \
}
XTLUA_IMGUI_TEXT(Text)
XTLUA_IMGUI_TEXT(TextDisabled)
XTLUA_IMGUI_TEXT(TextWrapped)
XTLUA_IMGUI_TEXT(BulletText)
XTLUA_IMGUI_TEXT(SetTooltip)
#undef XTLUA_IMGUI_TEXT

int extra_LabelText(lua_State* L)
{
  require_draw_frame(L);
  ImGui::LabelText(luaL_checkstring(L, 1), "%s", luaL_checkstring(L, 2));
  return 0;
}

int extra_TextColored(lua_State* L)
{
  require_draw_frame(L);
  ImGui::TextColored(ImVec4(check_float(L, 1), check_float(L, 2),
    check_float(L, 3), check_float(L, 4)), "%s", luaL_checkstring(L, 5));
  return 0;
}

#define XTLUA_IMGUI_EXTRA(name) { #name, extra_##name }
const luaL_Reg extra_imguilib[] = {
  XTLUA_IMGUI_EXTRA(BeginChild),
  XTLUA_IMGUI_EXTRA(SetNextWindowPos), XTLUA_IMGUI_EXTRA(SetNextWindowSize),
  XTLUA_IMGUI_EXTRA(SetNextWindowCollapsed),
  XTLUA_IMGUI_EXTRA(BeginCombo), XTLUA_IMGUI_EXTRA(BeginListBox),
  XTLUA_IMGUI_EXTRA(Selectable), XTLUA_IMGUI_EXTRA(BeginTable),
  XTLUA_IMGUI_EXTRA(TableNextRow), XTLUA_IMGUI_EXTRA(TableSetupColumn),
  XTLUA_IMGUI_EXTRA(BeginTabBar), XTLUA_IMGUI_EXTRA(BeginTabItem),
  XTLUA_IMGUI_EXTRA(TreeNodeEx), XTLUA_IMGUI_EXTRA(CollapsingHeader),
  XTLUA_IMGUI_EXTRA(SetNextItemOpen),
  XTLUA_IMGUI_EXTRA(DragFloat), XTLUA_IMGUI_EXTRA(DragInt),
  XTLUA_IMGUI_EXTRA(SliderFloat), XTLUA_IMGUI_EXTRA(SliderInt),
  XTLUA_IMGUI_EXTRA(InputFloat), XTLUA_IMGUI_EXTRA(InputInt),
  XTLUA_IMGUI_EXTRA(ProgressBar), XTLUA_IMGUI_EXTRA(InvisibleButton),
  XTLUA_IMGUI_EXTRA(IsWindowFocused), XTLUA_IMGUI_EXTRA(IsWindowHovered),
  XTLUA_IMGUI_EXTRA(IsItemHovered), XTLUA_IMGUI_EXTRA(TableGetColumnCount),
  XTLUA_IMGUI_EXTRA(TableGetColumnIndex), XTLUA_IMGUI_EXTRA(TableGetRowIndex),
  XTLUA_IMGUI_EXTRA(Text), XTLUA_IMGUI_EXTRA(TextDisabled),
  XTLUA_IMGUI_EXTRA(TextWrapped), XTLUA_IMGUI_EXTRA(BulletText),
  XTLUA_IMGUI_EXTRA(SetTooltip), XTLUA_IMGUI_EXTRA(LabelText),
  XTLUA_IMGUI_EXTRA(TextColored),
  { nullptr, nullptr }
};
#undef XTLUA_IMGUI_EXTRA

} // namespace


// THIS IS FOR LUA 5.3 although you can make a few changes for other versions



// define ENABLE_IM_LUA_END_STACK
// to keep track of end and begins and clean up the imgui stack
// if lua errors


// define this global before you call RunString or LoadImGuiBindings
lua_State* lState;

#ifdef ENABLE_IM_LUA_END_STACK
// Stack for imgui begin and end
std::deque<int> endStack;
static void AddToStack(int type) {
  endStack.push_back(type);
}

static void PopEndStack(int type) {
  if (!endStack.empty()) {
    endStack.pop_back(); // hopefully the type matches
  }
}

static void ImEndStack(int type);

#endif

// Example lua run string function
// returns NULL on success and error string on error
const char * RunString(const char* szLua) {
  if (!lState) {
    log_message(nullptr, "ImGui RunString requires an assigned Lua state\n");
    return "RunString requires an assigned Lua state";
  }

  int iStatus = luaL_loadstring(lState, szLua);
  if(iStatus) {
    return lua_tostring(lState, -1);
    //return;
  }
#ifdef ENABLE_IM_LUA_END_STACK
  endStack.clear();
#endif
  iStatus = lua_pcall( lState, 0, 0, 0 );

#ifdef ENABLE_IM_LUA_END_STACK
  bool wasEmpty = endStack.empty();
  while(!endStack.empty()) {
    ImEndStack(endStack.back());
    endStack.pop_back();
  }

#endif
  if( iStatus )
  {
      return lua_tostring(lState, -1);
      //return;
  }
#ifdef ENABLE_IM_LUA_END_STACK
  else if (!wasEmpty) {
    return "Script didn't clean up imgui stack properly";
  }
#endif
  return NULL;
}


#define IMGUI_FUNCTION_DRAW_LIST(name) \
static int impl_draw_list_##name(lua_State *L) { \
  if (!xtlua2_imgui_frame_active(L)) \
    return luaL_error(L, "imgui functions require an active xlua2_main draw callback"); \
  int max_args = lua_gettop(L); \
  int arg = 1; \
  int stackval = 0;

#define IMGUI_FUNCTION(name) \
static int impl_##name(lua_State *L) { \
  if (!xtlua2_imgui_frame_active(L)) \
    return luaL_error(L, "imgui functions require an active xlua2_main draw callback"); \
  int max_args = lua_gettop(L); \
  int arg = 1; \
  int stackval = 0;

// I use OpenGL so this is a GLuint
// Using unsigned int cause im lazy don't copy me
#define IM_TEXTURE_ID_ARG(name) \
  const ImTextureID name = (ImTextureID)luaL_checkinteger(L, arg++);

#define OPTIONAL_LABEL_ARG(name) \
  const char* name; \
  if (arg <= max_args) { \
    name = lua_tostring(L, arg++); \
  } else { \
    name = NULL; \
  }

#define LABEL_ARG(name) \
  size_t i_##name##_size; \
  const char * name = luaL_checklstring(L, arg++, &(i_##name##_size));

#define IM_VEC_2_ARG(name)\
  const lua_Number i_##name##_x = luaL_checknumber(L, arg++); \
  const lua_Number i_##name##_y = luaL_checknumber(L, arg++); \
  const ImVec2 name((double)i_##name##_x, (double)i_##name##_y);

#define OPTIONAL_IM_VEC_2_ARG(name, x, y) \
  lua_Number i_##name##_x = x; \
  lua_Number i_##name##_y = y; \
  if (arg <= max_args - 1) { \
    i_##name##_x = luaL_checknumber(L, arg++); \
    i_##name##_y = luaL_checknumber(L, arg++); \
  } \
  const ImVec2 name((double)i_##name##_x, (double)i_##name##_y);

#define IM_VEC_4_ARG(name) \
  const lua_Number i_##name##_x = luaL_checknumber(L, arg++); \
  const lua_Number i_##name##_y = luaL_checknumber(L, arg++); \
  const lua_Number i_##name##_z = luaL_checknumber(L, arg++); \
  const lua_Number i_##name##_w = luaL_checknumber(L, arg++); \
  const ImVec4 name((double)i_##name##_x, (double)i_##name##_y, (double)i_##name##_z, (double)i_##name##_w);

#define OPTIONAL_IM_VEC_4_ARG(name, x, y, z, w) \
  lua_Number i_##name##_x = x; \
  lua_Number i_##name##_y = y; \
  lua_Number i_##name##_z = z; \
  lua_Number i_##name##_w = w; \
  if (arg <= max_args - 1) { \
    i_##name##_x = luaL_checknumber(L, arg++); \
    i_##name##_y = luaL_checknumber(L, arg++); \
    i_##name##_z = luaL_checknumber(L, arg++); \
    i_##name##_w = luaL_checknumber(L, arg++); \
  } \
  const ImVec4 name((double)i_##name##_x, (double)i_##name##_y, (double)i_##name##_z, (double)i_##name##_w);

#define NUMBER_ARG(name)\
  lua_Number name = luaL_checknumber(L, arg++);

#define OPTIONAL_NUMBER_ARG(name, otherwise)\
  lua_Number name = otherwise; \
  if (arg <= max_args) { \
    name = lua_tonumber(L, arg++); \
  }

#define FLOAT_POINTER_ARG(name) \
  float i_##name##_value = luaL_checknumber(L, arg++); \
  float* name = &(i_##name##_value);

#define END_FLOAT_POINTER(name) \
  if (name != NULL) { \
    lua_pushnumber(L, i_##name##_value); \
    stackval++; \
  }

#define OPTIONAL_INT_ARG(name, otherwise)\
  int name = otherwise; \
  if (arg <= max_args) { \
    name = (int)lua_tonumber(L, arg++); \
  }

#define INT_ARG(name) \
  const int name = (int)luaL_checknumber(L, arg++);

#define OPTIONAL_UINT_ARG(name, otherwise)\
  unsigned int name = otherwise; \
  if (arg <= max_args) { \
    name = (unsigned int)lua_tounsigned(L, arg++); \
  }

#define UINT_ARG(name) \
  const unsigned int name = (unsigned int)luaL_checkinteger(L, arg++);

#define INT_POINTER_ARG(name) \
  int i_##name##_value = (int)luaL_checkinteger(L, arg++); \
  int* name = &(i_##name##_value);

#define END_INT_POINTER(name) \
  if (name != NULL) { \
    lua_pushnumber(L, i_##name##_value); \
    stackval++; \
  }

#define UINT_POINTER_ARG(name) \
  unsigned int i_##name##_value = (unsigned int)luaL_checkinteger(L, arg++); \
  unsigned int* name = &(i_##name##_value);

#define END_UINT_POINTER(name) \
  if (name != NULL) { \
    lua_pushnumber(L, i_##name##_value); \
    stackval++; \
  }

#define BOOL_POINTER_ARG(name) \
  bool i_##name##_value = lua_toboolean(L, arg++); \
  bool* name = &(i_##name##_value);

#define OPTIONAL_BOOL_POINTER_ARG(name) \
  bool i_##name##_value; \
  bool* name = NULL; \
  if (arg <= max_args) { \
    i_##name##_value = lua_toboolean(L, arg++); \
    name = &(i_##name##_value); \
  }

#define OPTIONAL_BOOL_ARG(name, otherwise) \
  bool name = otherwise; \
  if (arg <= max_args) { \
    name = lua_toboolean(L, arg++); \
  }

#define BOOL_ARG(name) \
  bool name = lua_toboolean(L, arg++);

#define CALL_FUNCTION(name, retType,...) \
  retType ret = ImGui::name(__VA_ARGS__);

#define DRAW_LIST_CALL_FUNCTION(name, retType,...) \
  retType ret = ImGui::GetWindowDrawList()->name(__VA_ARGS__);

#define CALL_FUNCTION_NO_RET(name, ...) \
  ImGui::name(__VA_ARGS__);

#define DRAW_LIST_CALL_FUNCTION_NO_RET(name, ...) \
  ImGui::GetWindowDrawList()->name(__VA_ARGS__);

#define PUSH_STRING(name) \
  lua_pushstring(L, name); \
  stackval++;

#define PUSH_NUMBER(name) \
  lua_pushnumber(L, name); \
  stackval++;

#define PUSH_BOOL(name) \
  lua_pushboolean(L, (int) name); \
  stackval++;

#define END_BOOL_POINTER(name) \
  if (name != NULL) { \
    lua_pushboolean(L, (int)i_##name##_value); \
    stackval++; \
  }

#define END_IMGUI_FUNC \
  return stackval; \
}

#ifdef ENABLE_IM_LUA_END_STACK
#define IF_RET_ADD_END_STACK(type) \
  if (ret) { \
    AddToStack(type); \
  }

#define ADD_END_STACK(type) \
  AddToStack(type);

#define POP_END_STACK(type) \
  PopEndStack(type);

#define END_STACK_START \
static void ImEndStack(int type) { \
  switch(type) {

#define END_STACK_OPTION(type, function) \
    case type: \
      ImGui::function(); \
      break;

#define END_STACK_END \
  } \
}
#else
#define END_STACK_START
#define END_STACK_OPTION(type, function)
#define END_STACK_END
#define IF_RET_ADD_END_STACK(type)
#define ADD_END_STACK(type)
#define POP_END_STACK(type)
#endif

#define START_ENUM(name)
#define MAKE_ENUM(c_name,lua_name)
#define END_ENUM(name)

#include "imgui_iterator.inl"


static const struct luaL_Reg imguilib [] = {
#undef IMGUI_FUNCTION
#define IMGUI_FUNCTION(name) {#name, impl_##name},
#undef IMGUI_FUNCTION_DRAW_LIST
#define IMGUI_FUNCTION_DRAW_LIST(name) {"DrawList_" #name, impl_draw_list_##name},
// These defines are just redefining everything to nothing so
// we can get the function names
#undef IM_TEXTURE_ID_ARG
#define IM_TEXTURE_ID_ARG(name)
#undef OPTIONAL_LABEL_ARG
#define OPTIONAL_LABEL_ARG(name)
#undef LABEL_ARG
#define LABEL_ARG(name)
#undef IM_VEC_2_ARG
#define IM_VEC_2_ARG(name)
#undef OPTIONAL_IM_VEC_2_ARG
#define OPTIONAL_IM_VEC_2_ARG(name, x, y)
#undef IM_VEC_4_ARG
#define IM_VEC_4_ARG(name)
#undef OPTIONAL_IM_VEC_4_ARG
#define OPTIONAL_IM_VEC_4_ARG(name, x, y, z, w)
#undef NUMBER_ARG
#define NUMBER_ARG(name)
#undef OPTIONAL_NUMBER_ARG
#define OPTIONAL_NUMBER_ARG(name, otherwise)
#undef FLOAT_POINTER_ARG
#define FLOAT_POINTER_ARG(name)
#undef END_FLOAT_POINTER
#define END_FLOAT_POINTER(name)
#undef OPTIONAL_INT_ARG
#define OPTIONAL_INT_ARG(name, otherwise)
#undef INT_ARG
#define INT_ARG(name)
#undef OPTIONAL_UINT_ARG
#define OPTIONAL_UINT_ARG(name, otherwise)
#undef UINT_ARG
#define UINT_ARG(name)
#undef INT_POINTER_ARG
#define INT_POINTER_ARG(name)
#undef END_INT_POINTER
#define END_INT_POINTER(name)
#undef UINT_POINTER_ARG
#define UINT_POINTER_ARG(name)
#undef END_UINT_POINTER
#define END_UINT_POINTER(name)
#undef BOOL_POINTER_ARG
#define BOOL_POINTER_ARG(name)
#undef OPTIONAL_BOOL_POINTER_ARG
#define OPTIONAL_BOOL_POINTER_ARG(name)
#undef OPTIONAL_BOOL_ARG
#define OPTIONAL_BOOL_ARG(name, otherwise)
#undef BOOL_ARG
#define BOOL_ARG(name)
#undef CALL_FUNCTION
#define CALL_FUNCTION(name, retType, ...)
#undef DRAW_LIST_CALL_FUNCTION
#define DRAW_LIST_CALL_FUNCTION(name, retType, ...)
#undef CALL_FUNCTION_NO_RET
#define CALL_FUNCTION_NO_RET(name, ...)
#undef DRAW_LIST_CALL_FUNCTION_NO_RET
#define DRAW_LIST_CALL_FUNCTION_NO_RET(name, ...)
#undef PUSH_STRING
#define PUSH_STRING(name)
#undef PUSH_NUMBER
#define PUSH_NUMBER(name)
#undef PUSH_BOOL
#define PUSH_BOOL(name)
#undef END_BOOL_POINTER
#define END_BOOL_POINTER(name)
#undef END_IMGUI_FUNC
#define END_IMGUI_FUNC
#undef END_STACK_START
#define END_STACK_START
#undef END_STACK_OPTION
#define END_STACK_OPTION(type, function)
#undef END_STACK_END
#define END_STACK_END
#undef IF_RET_ADD_END_STACK
#define IF_RET_ADD_END_STACK(type)
#undef ADD_END_STACK
#define ADD_END_STACK(type)
#undef POP_END_STACK
#define POP_END_STACK(type)
#undef START_ENUM
#define START_ENUM(name)
#undef MAKE_ENUM
#define MAKE_ENUM(c_name,lua_name)
#undef END_ENUM
#define END_ENUM(name)

#include "imgui_iterator.inl"
  {"Button", impl_Button},
  {NULL, NULL}
};

static void PushImguiEnums(lua_State* lState, const char* tableName) {
  lua_pushstring(lState, tableName);
  lua_newtable(lState);

#undef START_ENUM
#undef MAKE_ENUM
#undef END_ENUM
#define START_ENUM(name) \
  lua_pushstring(lState, #name); \
  lua_newtable(lState); \
  { \
    int i = 1;
#define MAKE_ENUM(c_name,lua_name) \
  lua_pushstring(lState, #lua_name); \
  lua_pushnumber(lState, c_name); \
  lua_rawset(lState, -3);
#define END_ENUM(name) \
  } \
  lua_rawset(lState, -3);
// These defines are just redefining everything to nothing so
// we get only the enums.
#undef IMGUI_FUNCTION
#define IMGUI_FUNCTION(name)
#undef IMGUI_FUNCTION_DRAW_LIST
#define IMGUI_FUNCTION_DRAW_LIST(name)
#undef IM_TEXTURE_ID_ARG
#define IM_TEXTURE_ID_ARG(name)
#undef OPTIONAL_LABEL_ARG
#define OPTIONAL_LABEL_ARG(name)
#undef LABEL_ARG
#define LABEL_ARG(name)
#undef IM_VEC_2_ARG
#define IM_VEC_2_ARG(name)
#undef OPTIONAL_IM_VEC_2_ARG
#define OPTIONAL_IM_VEC_2_ARG(name, x, y)
#undef IM_VEC_4_ARG
#define IM_VEC_4_ARG(name)
#undef OPTIONAL_IM_VEC_4_ARG
#define OPTIONAL_IM_VEC_4_ARG(name, x, y, z, w)
#undef NUMBER_ARG
#define NUMBER_ARG(name)
#undef OPTIONAL_NUMBER_ARG
#define OPTIONAL_NUMBER_ARG(name, otherwise)
#undef FLOAT_POINTER_ARG
#define FLOAT_POINTER_ARG(name)
#undef END_FLOAT_POINTER
#define END_FLOAT_POINTER(name)
#undef OPTIONAL_INT_ARG
#define OPTIONAL_INT_ARG(name, otherwise)
#undef INT_ARG
#define INT_ARG(name)
#undef OPTIONAL_UINT_ARG
#define OPTIONAL_UINT_ARG(name, otherwise)
#undef UINT_ARG
#define UINT_ARG(name)
#undef INT_POINTER_ARG
#define INT_POINTER_ARG(name)
#undef END_INT_POINTER
#define END_INT_POINTER(name)
#undef UINT_POINTER_ARG
#define UINT_POINTER_ARG(name)
#undef END_UINT_POINTER
#define END_UINT_POINTER(name)
#undef BOOL_POINTER_ARG
#define BOOL_POINTER_ARG(name)
#undef OPTIONAL_BOOL_POINTER_ARG
#define OPTIONAL_BOOL_POINTER_ARG(name)
#undef OPTIONAL_BOOL_ARG
#define OPTIONAL_BOOL_ARG(name, otherwise)
#undef BOOL_ARG
#define BOOL_ARG(name)
#undef CALL_FUNCTION
#define CALL_FUNCTION(name, retType, ...)
#undef DRAW_LIST_CALL_FUNCTION
#define DRAW_LIST_CALL_FUNCTION(name, retType, ...)
#undef CALL_FUNCTION_NO_RET
#define CALL_FUNCTION_NO_RET(name, ...)
#undef DRAW_LIST_CALL_FUNCTION_NO_RET
#define DRAW_LIST_CALL_FUNCTION_NO_RET(name, ...)
#undef PUSH_STRING
#define PUSH_STRING(name)
#undef PUSH_NUMBER
#define PUSH_NUMBER(name)
#undef PUSH_BOOL
#define PUSH_BOOL(name)
#undef END_BOOL_POINTER
#define END_BOOL_POINTER(name)
#undef END_IMGUI_FUNC
#define END_IMGUI_FUNC
#undef END_STACK_START
#define END_STACK_START
#undef END_STACK_OPTION
#define END_STACK_OPTION(type, function)
#undef END_STACK_END
#define END_STACK_END
#undef IF_RET_ADD_END_STACK
#define IF_RET_ADD_END_STACK(type)
#undef ADD_END_STACK
#define ADD_END_STACK(type)
#undef POP_END_STACK
#define POP_END_STACK(type)

#include "imgui_iterator.inl"

  lua_rawset(lState, -3);
};


// Registration is per Lua state. RunString retains its historical global
// lState, but the XTLua xlua2_main host never uses that example helper.
void LoadImguiBindings(lua_State* L) {
  lua_newtable(L);
  luaL_setfuncs(L, imguilib, 0);
  luaL_setfuncs(L, extra_imguilib, 0);
  // The host owns frame lifetime. Letting Lua call EndFrame() here would
  // invalidate the frame before the panel draw callback renders it.
  lua_pushnil(L);
  lua_setfield(L, -2, "EndFrame");
  PushImguiEnums(L, "constant");
  lua_setglobal(L, "imgui");
}
