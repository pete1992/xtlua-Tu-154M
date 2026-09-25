#ifndef XTLUA_SHARED_XPFUNCS_H
#define XTLUA_SHARED_XPFUNCS_H

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

// Shared callback storage used by the SDK 4.4 generated bindings.  These
// callbacks are only installed in XLua 2 modules, which always run on the
// X-Plane thread.
class notify_cb_t {
public:
	notify_cb_t() = delete;
	notify_cb_t(lua_State * inL, int registry_index);
	~notify_cb_t();

	int get_capture() const { return m_capture_registry_index; }

	lua_State * L = nullptr;
	std::map<std::string, int> callbacks;

	static constexpr int kNeverPersist = 0;

private:
	int m_capture_registry_index = kNeverPersist;
	static int s_nil_ref_count;
};

std::string get_log_prefix(char level = 'I');
std::filesystem::path get_current_script_path(lua_State * L);
int log_message(lua_State * L, const char * format, ...);

std::shared_ptr<notify_cb_t> capture_lua_value(lua_State * L, int index);
std::shared_ptr<notify_cb_t> wrap_lua_func_no_userref(
	lua_State * L,
	int index,
	const std::string& callback_key);
bool wrap_next_lua_func(
	std::shared_ptr<notify_cb_t> callback,
	int function_index,
	bool optional,
	const std::string& callback_key);
lua_State * setup_lua_callback(
	const notify_cb_t * callback,
	const std::string callback_key);

void xlua_persist_userref(lua_State * L, std::shared_ptr<notify_cb_t> callback);
void xlua_remove_callback(std::shared_ptr<notify_cb_t> callback);
void xlua_remove_callback(const notify_cb_t * callback);
void xlua_callback_cleanup(lua_State * L);
void xlua_callback_shutdown();
bool xlua_is_callback_valid(const notify_cb_t * callback);
void xlua_install_panic_handler(lua_State * L);

std::optional<std::string> xlua_checkoptstring(lua_State * L, int narg);
std::optional<float> xlua_checkoptfloat(lua_State * L, int narg);
std::optional<double> xlua_checkoptdouble(lua_State * L, int narg);
std::optional<int> xlua_checkoptint(lua_State * L, int narg);

inline bool xlua_checkboolean(lua_State * L, int narg)
{
	luaL_checktype(L, narg, LUA_TBOOLEAN);
	return lua_toboolean(L, narg) != 0;
}

inline int xlua_checkinteger(lua_State * L, int narg)
{
	return static_cast<int>(luaL_checkinteger(L, narg));
}

inline lua_Number xlua_checknumber(lua_State * L, int narg)
{
	return luaL_checknumber(L, narg);
}

inline const char * xlua_checkstring(lua_State * L, int narg)
{
	return luaL_checkstring(L, narg);
}

inline uint8_t xlua_checkbyte(lua_State * L, int narg)
{
	return static_cast<uint8_t>((std::clamp)(
		luaL_checkinteger(L, narg),
		static_cast<lua_Integer>(0),
		static_cast<lua_Integer>(255)));
}

inline int xlua_tointeger(lua_State * L, int narg)
{
	return static_cast<int>(lua_tointeger(L, narg));
}

inline lua_Number xlua_tonumber(lua_State * L, int narg)
{
	return lua_tonumber(L, narg);
}

inline uint8_t xlua_tobyte(lua_State * L, int narg)
{
	return static_cast<uint8_t>((std::clamp)(
		lua_tointeger(L, narg),
		static_cast<lua_Integer>(0),
		static_cast<lua_Integer>(255)));
}

inline void xlua_pushinteger(lua_State * L, lua_Integer value)
{
	lua_pushinteger(L, value);
}

inline void xlua_pushnumber(lua_State * L, lua_Number value)
{
	lua_pushnumber(L, value);
}

inline void xlua_pushbyte(lua_State * L, lua_Integer value)
{
	lua_pushinteger(L, (std::clamp)(
		value,
		static_cast<lua_Integer>(0),
		static_cast<lua_Integer>(255)));
}

inline bool xlua_requireuserdata(lua_State * L, int narg)
{
	if(lua_type(L, narg) != LUA_TUSERDATA)
	{
		luaL_argerror(L, narg, "expected SDK userdata handle");
		return false;
	}

	// Every generated handle parameter currently has pointer width. Reject a
	// typed null handle at call sites, while xlua_checkuserdata itself still
	// permits null sentinels for generated __eq functions (so failed lookup
	// results can be compared safely in Lua).
	if(lua_objlen(L, narg) == sizeof(void *))
	{
		void * handle = nullptr;
		std::memcpy(&handle, lua_touserdata(L, narg), sizeof(handle));
		if(handle == nullptr)
		{
			luaL_argerror(L, narg, "SDK handle must not be null");
			return false;
		}
	}
	return true;
}

template <typename T>
T xlua_checkuserdata(lua_State * L, int narg, const char * message)
{
	// Generated SDK handles are stored by value in full userdata.  Accepting
	// light userdata (or a too-small, unrelated userdata block) would turn an
	// arbitrary pointer into a C++ read here.
	if(lua_type(L, narg) != LUA_TUSERDATA ||
	   lua_objlen(L, narg) < sizeof(T))
	{
		luaL_argerror(L, narg, message);
		return T{};
	}

	// When the generated type has a registered metatable, require that exact
	// type as well. Raw SDK pointer payloads (for example void* message params)
	// and host-private userdata intentionally have no generated metatable and
	// therefore retain the size-checked path above.
	static const char expected_prefix[] = "Expected ";
	if(message != nullptr &&
	   std::strncmp(message, expected_prefix, sizeof(expected_prefix) - 1) == 0)
	{
		const std::string metatable_name =
			std::string("_mt_") +
			(message + sizeof(expected_prefix) - 1);
		luaL_getmetatable(L, metatable_name.c_str());
		if(!lua_isnil(L, -1))
		{
			const bool has_metatable = lua_getmetatable(L, narg) != 0;
			const bool matches = has_metatable &&
				lua_rawequal(L, -1, -2) != 0;
			lua_pop(L, has_metatable ? 2 : 1);
			if(!matches)
			{
				luaL_argerror(L, narg, message);
				return T{};
			}
		}
		else
		{
			lua_pop(L, 1);
		}
	}

	T * value = static_cast<T *>(lua_touserdata(L, narg));
	if(value == nullptr)
	{
		luaL_argerror(L, narg, message);
		return T{};
	}
	return *value;
}

template <typename T>
void xlua_pushuserdata(lua_State * L, T value)
{
	T * userdata = static_cast<T *>(lua_newuserdata(L, sizeof(T)));
	std::memcpy(userdata, &value, sizeof(T));
}

#endif
