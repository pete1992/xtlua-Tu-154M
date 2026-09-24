#include "shared_xpfuncs.h"

#include "module.h"

#include <XPLMUtilities.h>

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

int notify_cb_t::s_nil_ref_count = -100;

notify_cb_t::notify_cb_t(lua_State * inL, int registry_index) :
	L(inL),
	m_capture_registry_index(registry_index)
{
	if(m_capture_registry_index == LUA_REFNIL)
		m_capture_registry_index = --s_nil_ref_count;
	else if(m_capture_registry_index != kNeverPersist &&
		m_capture_registry_index < 0)
	{
		assert(false);
		m_capture_registry_index = kNeverPersist;
	}
}

notify_cb_t::~notify_cb_t()
{
	if(L == nullptr)
		return;

	if(m_capture_registry_index > 0)
		luaL_unref(L, LUA_REGISTRYINDEX, m_capture_registry_index);

	for(const auto& callback : callbacks)
	{
		if(callback.second > 0)
			luaL_unref(L, LUA_REGISTRYINDEX, callback.second);
	}
}

namespace {

// C++17-compatible ownership registry.  The raw pointer is exactly the refcon
// handed to XPLM; the shared_ptr value keeps the Lua registry references alive.
std::unordered_map<const notify_cb_t *, std::shared_ptr<notify_cb_t>> g_callbacks;

// The queue owns only text. In particular, draining finalizer/unload messages
// never dereferences a module or Lua state which has already been destroyed.
std::mutex g_log_mutex;
std::deque<std::string> g_log_queue;
std::thread::id g_log_main_thread;
bool g_log_draining = false; // Protected by g_log_mutex, including reentry.

struct log_drain_guard {

	~log_drain_guard()
	{
		std::lock_guard<std::mutex> lock(g_log_mutex);
		g_log_draining = false;
	}
};

int panic_handler(lua_State * L)
{
	const char * message = lua_tostring(L, -1);
	log_message(L, "FATAL: unprotected Lua error: %s\n",
		message ? message : "(no message on stack)");
	return 0;
}

} // namespace

void xtlua_log_set_main_thread()
{
	std::lock_guard<std::mutex> lock(g_log_mutex);
	g_log_main_thread = std::this_thread::get_id();
}

void xtlua_queue_log(std::string message)
{
	std::lock_guard<std::mutex> lock(g_log_mutex);
	g_log_queue.push_back(std::move(message));
}

std::size_t xtlua_flush_log_queue()
{
	std::deque<std::string> batch;
	{
		std::lock_guard<std::mutex> lock(g_log_mutex);
		if(g_log_main_thread != std::this_thread::get_id() ||
		   g_log_draining || g_log_queue.empty())
			return 0;
		g_log_draining = true;
		batch.swap(g_log_queue);
	}

	const log_drain_guard guard;
	for(const std::string& message : batch)
		XPLMDebugString(message.c_str());
	return batch.size();
}

std::string get_log_prefix(char level)
{
	std::string prefix("XTLua ");
	prefix.push_back(level);
	prefix += ": ";
	return prefix;
}

std::filesystem::path get_current_script_path(lua_State * L)
{
	module * owner = module::module_from_interp(L);
	return owner ? std::filesystem::path(owner->get_script_path()) :
		std::filesystem::path();
}

int log_message(lua_State * L, const char * format, ...)
{
	char buffer[2048];
	va_list args;
	va_start(args, format);
	const int result = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	buffer[sizeof(buffer) - 1] = 0;

	std::string output = get_log_prefix(L ? 'E' : 'I');
	if(L)
	{
		module * owner = module::module_from_interp(L);
		if(owner != nullptr)
		{
			switch(owner->get_runtime())
			{
			case module_runtime::xtlua_worker: output += "xtlua_worker: "; break;
			case module_runtime::xtlua_main: output += "xtlua_main: "; break;
			case module_runtime::xlua2_main: output += "xlua2_main: "; break;
			}
		}
		const std::filesystem::path script = get_current_script_path(L);
		if(!script.empty())
		{
			output += script.generic_string();
			output += ": ";
		}
	}
	output += buffer;
	xtlua_queue_log(std::move(output));
	return result;
}

std::shared_ptr<notify_cb_t> capture_lua_value(lua_State * L, int index)
{
	lua_pushvalue(L, index);
	return std::make_shared<notify_cb_t>(
		L,
		luaL_ref(L, LUA_REGISTRYINDEX));
}

bool wrap_next_lua_func(
	std::shared_ptr<notify_cb_t> callback,
	int function_index,
	bool optional,
	const std::string& callback_key)
{
	if(!callback)
		return false;

	if(!lua_isfunction(callback->L, function_index) &&
		!lua_isnil(callback->L, function_index))
	{
		const std::string message = callback_key +
			" callback must be a function or nil";
		luaL_argerror(callback->L, function_index, message.c_str());
		return false;
	}

	if(!optional && lua_isnil(callback->L, function_index))
	{
		const std::string message = callback_key +
			" callback must be a function";
		luaL_argerror(callback->L, function_index, message.c_str());
		return false;
	}

	lua_pushvalue(callback->L, function_index);
	callback->callbacks[callback_key] =
		luaL_ref(callback->L, LUA_REGISTRYINDEX);
	return callback->callbacks[callback_key] != LUA_REFNIL;
}

std::shared_ptr<notify_cb_t> wrap_lua_func_no_userref(
	lua_State * L,
	int index,
	const std::string& callback_key)
{
	if(lua_isnil(L, index))
		return nullptr;

	auto callback = std::make_shared<notify_cb_t>(
		L,
		notify_cb_t::kNeverPersist);
	wrap_next_lua_func(callback, index, false, callback_key);
	return callback;
}

lua_State * setup_lua_callback(
	const notify_cb_t * callback,
	const std::string callback_key)
{
	if(callback == nullptr || callback->L == nullptr)
		return nullptr;

	module * owner = module::module_from_interp(callback->L);
	if(owner == nullptr)
		return nullptr;
	if(owner->is_xlua2() && !owner->is_enabled())
		return nullptr;

	if(callback->get_capture() != notify_cb_t::kNeverPersist &&
		!xlua_is_callback_valid(callback))
	{
		log_message(nullptr,
			"invalid callback '%s' was invoked after cleanup\n",
			callback_key.c_str());
		return nullptr;
	}

	const auto stored = callback->callbacks.find(callback_key);
	if(stored == callback->callbacks.end())
	{
		log_message(callback->L,
			"callback '%s' is not registered\n",
			callback_key.c_str());
		return nullptr;
	}
	if(stored->second == LUA_REFNIL)
		return nullptr;

	lua_rawgeti(callback->L, LUA_REGISTRYINDEX, stored->second);
	if(lua_isfunction(callback->L, -1))
		return callback->L;

	lua_pop(callback->L, 1);
	log_message(callback->L,
		"callback '%s' no longer resolves to a function\n",
		callback_key.c_str());
	return nullptr;
}

void xlua_persist_userref(
	lua_State * L,
	std::shared_ptr<notify_cb_t> callback)
{
	if(!callback || callback->get_capture() == notify_cb_t::kNeverPersist)
		return;

	g_callbacks[callback.get()] = callback;
	size_t live_for_state = 0;
	for(const auto& registered : g_callbacks)
	{
		if(registered.second && registered.second->L == L)
			++live_for_state;
	}
	if(live_for_state > 500)
	{
		luaL_error(L,
			"%s has persisted more than 500 callbacks",
			get_current_script_path(L).generic_string().c_str());
	}
}

void xlua_remove_callback(std::shared_ptr<notify_cb_t> callback)
{
	if(callback)
		g_callbacks.erase(callback.get());
}

void xlua_remove_callback(const notify_cb_t * callback)
{
	if(callback)
		g_callbacks.erase(callback);
}

bool xlua_is_callback_valid(const notify_cb_t * callback)
{
	return callback != nullptr && g_callbacks.find(callback) != g_callbacks.end();
}

void xlua_callback_cleanup(lua_State * L)
{
	for(auto& callback : g_callbacks)
	{
		if(callback.second && callback.second->L == L)
		{
			// XPLM may still own the raw notify_cb_t refcon for a resource the
			// script failed to destroy. Quarantine the record until the whole
			// plugin shuts down so a late callback becomes a safe no-op rather
			// than dereferencing freed memory or a closed lua_State.
			callback.second->L = nullptr;
		}
	}
}

void xlua_callback_shutdown()
{
	for(auto& callback : g_callbacks)
	{
		if(callback.second)
			callback.second->L = nullptr;
	}
	g_callbacks.clear();
}

std::optional<std::string> xlua_checkoptstring(lua_State * L, int narg)
{
	if(lua_isnil(L, narg))
		return std::nullopt;
	return std::string(luaL_checkstring(L, narg));
}

std::optional<float> xlua_checkoptfloat(lua_State * L, int narg)
{
	if(lua_isnil(L, narg))
		return std::nullopt;
	return static_cast<float>(luaL_checknumber(L, narg));
}

std::optional<double> xlua_checkoptdouble(lua_State * L, int narg)
{
	if(lua_isnil(L, narg))
		return std::nullopt;
	return static_cast<double>(luaL_checknumber(L, narg));
}

std::optional<int> xlua_checkoptint(lua_State * L, int narg)
{
	if(lua_isnil(L, narg))
		return std::nullopt;
	return static_cast<int>(luaL_checkinteger(L, narg));
}

void xlua_install_panic_handler(lua_State * L)
{
	lua_atpanic(L, panic_handler);
}
