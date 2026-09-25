#include "xtlua_render_bridge.h"

#include "module.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr size_t kMaxChannelLength = 128;
constexpr size_t kMaxChannels = 32;
constexpr size_t kMaxFields = 128;
constexpr size_t kMaxArrayValues = 4096;
constexpr size_t kMaxTextBytes = 65536;

using render_value = std::variant<double, bool, std::string, std::vector<double>>;

struct render_frame {
	uint64_t sequence = 0;
	std::map<std::string, render_value> fields;
};

using render_frames = std::map<std::string, std::shared_ptr<const render_frame>>;

// An immutable map and immutable frames are published by pointer swap. The
// main-thread reader never waits for worker Lua or holds a DataRef/SDK lock.
std::shared_ptr<const render_frames> render_buffer =
	std::make_shared<const render_frames>();
std::atomic<uint64_t> next_sequence{1};

enum class parse_error {
	none,
	field_count,
	field_name,
	field_type,
	array_size,
	array_value,
	text_size
};

parse_error copy_lua_frame(lua_State * L, render_frame& frame)
{
	size_t text_bytes = 0;
	lua_pushnil(L);
	while(lua_next(L, 2) != 0)
	{
		if(frame.fields.size() >= kMaxFields)
		{
			lua_pop(L, 2);
			return parse_error::field_count;
		}
		if(lua_type(L, -2) != LUA_TSTRING)
		{
			lua_pop(L, 2);
			return parse_error::field_name;
		}
		size_t key_length = 0;
		const char * key = lua_tolstring(L, -2, &key_length);
		if(key_length == 0 || key_length > kMaxChannelLength ||
			std::string(key, key_length).find('\0') != std::string::npos)
		{
			lua_pop(L, 2);
			return parse_error::field_name;
		}
		std::string name(key, key_length);
		switch(lua_type(L, -1))
		{
		case LUA_TNUMBER:
		{
			const double value = lua_tonumber(L, -1);
			if(!std::isfinite(value))
			{
				lua_pop(L, 2);
				return parse_error::field_type;
			}
			frame.fields.emplace(std::move(name), value);
			break;
		}
		case LUA_TBOOLEAN:
			frame.fields.emplace(std::move(name), lua_toboolean(L, -1) != 0);
			break;
		case LUA_TSTRING:
		{
			size_t length = 0;
			const char * value = lua_tolstring(L, -1, &length);
			if(length > kMaxTextBytes - text_bytes)
			{
				lua_pop(L, 2);
				return parse_error::text_size;
			}
			text_bytes += length;
			frame.fields.emplace(std::move(name), std::string(value, length));
			break;
		}
		case LUA_TTABLE:
		{
			const size_t count = lua_objlen(L, -1);
			if(count > kMaxArrayValues)
			{
				lua_pop(L, 2);
				return parse_error::array_size;
			}
			std::vector<double> values;
			values.reserve(count);
			for(size_t i = 1; i <= count; ++i)
			{
				lua_rawgeti(L, -1, static_cast<int>(i));
				if(lua_type(L, -1) != LUA_TNUMBER ||
					!std::isfinite(lua_tonumber(L, -1)))
				{
					lua_pop(L, 3);
					return parse_error::array_value;
				}
				values.push_back(lua_tonumber(L, -1));
				lua_pop(L, 1);
			}
			const int array_index = lua_gettop(L);
			lua_pushnil(L);
			size_t seen = 0;
			while(lua_next(L, array_index) != 0)
			{
				const double key_number = lua_tonumber(L, -2);
				const bool valid_key = lua_type(L, -2) == LUA_TNUMBER &&
					std::isfinite(key_number) && key_number >= 1.0 &&
					key_number <= static_cast<double>(count) &&
					key_number == std::floor(key_number);
				if(!valid_key)
				{
					lua_pop(L, 4); // Inner value/key, then outer value/key.
					return parse_error::array_value;
				}
				++seen;
				lua_pop(L, 1);
			}
			if(seen != count)
			{
				lua_pop(L, 2);
				return parse_error::array_value;
			}
			frame.fields.emplace(std::move(name), std::move(values));
			break;
		}
		default:
			lua_pop(L, 2);
			return parse_error::field_type;
		}
		lua_pop(L, 1); // Retain the key for lua_next().
	}
	return parse_error::none;
}

const char * error_text(parse_error error)
{
	switch(error)
	{
	case parse_error::field_count: return "render buffer has too many fields";
	case parse_error::field_name: return "render buffer keys must be short strings";
	case parse_error::field_type: return "render buffer values must be finite numbers, booleans, strings or numeric arrays";
	case parse_error::array_size: return "render buffer numeric array is too large";
	case parse_error::array_value: return "render buffer arrays must be dense numeric sequences";
	case parse_error::text_size: return "render buffer text is too large";
	case parse_error::none: break;
	}
	return "invalid render buffer";
}

int publish_render_buffer(lua_State * L)
{
	if(lua_type(L, 1) != LUA_TSTRING || lua_type(L, 2) != LUA_TTABLE)
		return luaL_error(L, "XTLuaPublishRenderBuffer(channel, state) expected");
	size_t channel_length = 0;
	const char * channel = lua_tolstring(L, 1, &channel_length);
	if(channel_length == 0 || channel_length > kMaxChannelLength)
		return luaL_error(L, "render buffer channel must be 1..128 bytes");

	auto worker_buffer = std::make_shared<render_frame>();
	const parse_error error = copy_lua_frame(L, *worker_buffer);
	if(error != parse_error::none)
	{
		lua_pushnil(L);
		lua_pushstring(L, error_text(error));
		return 2;
	}
	worker_buffer->sequence = next_sequence.fetch_add(1, std::memory_order_relaxed);
	const std::string channel_name(channel, channel_length);
	for(;;)
	{
		auto current = std::atomic_load_explicit(
			&render_buffer, std::memory_order_acquire);
		// A publisher that built an older frame must not replace a newer one
		// if multiple worker-side publishers are ever introduced.
		const auto existing = current->find(channel_name);
		if(existing != current->end() &&
			existing->second->sequence >= worker_buffer->sequence)
		{
			lua_pushnumber(L, static_cast<lua_Number>(existing->second->sequence));
			return 1;
		}
		if(current->size() >= kMaxChannels &&
			current->find(channel_name) == current->end())
		{
			lua_pushnil(L);
			lua_pushstring(L, "too many render buffer channels");
			return 2;
		}
		auto updated = std::make_shared<render_frames>(*current);
		(*updated)[channel_name] = worker_buffer;
		std::shared_ptr<const render_frames> desired = updated;
		if(std::atomic_compare_exchange_weak_explicit(
				&render_buffer, &current, desired,
				std::memory_order_release, std::memory_order_acquire))
			break;
	}
	lua_pushnumber(L, static_cast<lua_Number>(worker_buffer->sequence));
	return 1;
}

void push_render_value(lua_State * L, const render_value& value)
{
	if(const auto * number = std::get_if<double>(&value))
		lua_pushnumber(L, *number);
	else if(const auto * boolean = std::get_if<bool>(&value))
		lua_pushboolean(L, *boolean);
	else if(const auto * text = std::get_if<std::string>(&value))
		lua_pushlstring(L, text->data(), text->size());
	else if(const auto * numbers = std::get_if<std::vector<double>>(&value))
	{
		lua_createtable(L, static_cast<int>(numbers->size()), 0);
		for(size_t i = 0; i < numbers->size(); ++i)
		{
			lua_pushnumber(L, (*numbers)[i]);
			lua_rawseti(L, -2, static_cast<int>(i + 1));
		}
	}
}

int get_render_buffer(lua_State * L)
{
	if(lua_type(L, 1) != LUA_TSTRING)
		return luaL_error(L, "XLuaGetRenderBuffer(channel) expected");
	size_t channel_length = 0;
	const char * channel = lua_tolstring(L, 1, &channel_length);
	auto frames = std::atomic_load_explicit(
		&render_buffer, std::memory_order_acquire);
	const auto found = frames->find(std::string(channel, channel_length));
	if(found == frames->end())
	{
		lua_pushnil(L);
		return 1;
	}
	const auto frame = found->second;
	lua_createtable(L, 0, static_cast<int>(frame->fields.size()));
	for(const auto& field : frame->fields)
	{
		push_render_value(L, field.second);
		lua_setfield(L, -2, field.first.c_str());
	}
	lua_pushnumber(L, static_cast<lua_Number>(frame->sequence));
	return 2;
}

} // namespace

void xtlua_register_render_bridge(lua_State * L, module_runtime runtime)
{
	if(runtime == module_runtime::xtlua_worker)
		lua_register(L, "XTLuaPublishRenderBuffer", publish_render_buffer);
	else if(runtime == module_runtime::xlua2_main)
		lua_register(L, "XLuaGetRenderBuffer", get_render_buffer);
}

void xtlua_clear_render_bridge()
{
	std::shared_ptr<const render_frames> empty =
		std::make_shared<const render_frames>();
	std::atomic_store_explicit(
		&render_buffer, std::move(empty), std::memory_order_release);
}
