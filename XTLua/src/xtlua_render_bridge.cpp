#include "xtlua_render_bridge.h"

#include "module.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
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

constexpr uint32_t kRenderSlotCount = 3;
constexpr uint32_t kSlotIndexMask = 0x3u;
constexpr uint32_t kPublishedFrame = 0x80000000u;

static_assert(std::atomic<uint32_t>::is_always_lock_free,
	"XTLua's render bridge requires lock-free 32-bit atomics");

using render_value = std::variant<double, bool, std::string, std::vector<double>>;

struct render_frame {
	uint64_t sequence = 0;
	std::map<std::string, render_value> fields;
};

// One instance has exactly one producer (xtlua_worker) and one consumer
// (X-Plane's main thread). Slot ownership is transferred through middle_state:
//
//   worker_buffer --publish()--> middle --acquire()--> render_buffer
//
// The third slot lets either side keep its current buffer while the other side
// advances. Intermediate published frames may be replaced; neither side waits.
class render_channel {
public:
	bool matches(const char * bytes, size_t length) const
	{
		return name.size() == length &&
			name.compare(0, name.size(), bytes, length) == 0;
	}

	void prepare(const char * bytes, size_t length)
	{
		reset();
		name.assign(bytes, length);
	}

	render_frame& worker_buffer()
	{
		return buffers[worker_slot];
	}

	void publish_worker_buffer()
	{
		// Release publishes the completed worker buffer. Acquire makes the
		// returned slot writable only after the consumer released it.
		const uint32_t previous = middle_state.exchange(
			worker_slot | kPublishedFrame, std::memory_order_acq_rel);
		worker_slot = previous & kSlotIndexMask;
	}

	void acquire_render_buffer()
	{
		if((middle_state.load(std::memory_order_acquire) &
			kPublishedFrame) != 0)
		{
			// Release returns the old render slot to the producer. Acquire
			// makes the newly published frame visible before it is copied to
			// the main-thread Lua state.
			const uint32_t previous = middle_state.exchange(
				render_slot, std::memory_order_acq_rel);
			render_slot = previous & kSlotIndexMask;
		}
	}

	const render_frame * render_buffer() const
	{
		const render_frame& frame = buffers[render_slot];
		return frame.sequence == 0 ? nullptr : &frame;
	}

	void reset()
	{
		name.clear();
		for(render_frame& frame : buffers)
		{
			frame.sequence = 0;
			frame.fields.clear();
		}
		render_slot = 0;
		worker_slot = 2;
		middle_state.store(1, std::memory_order_relaxed);
	}

private:
	std::string name;
	std::array<render_frame, kRenderSlotCount> buffers;

	// The low two bits identify the middle slot. The high bit means that slot
	// contains a completed frame which the consumer has not acquired yet.
	std::atomic<uint32_t> middle_state{1};

	// Accessed by only one side each; their slots are never shared.
	uint32_t render_slot = 0;
	uint32_t worker_slot = 2;
};

std::array<render_channel, kMaxChannels> channels;

// A channel name is immutable after this release publication. Only the worker
// adds channels; only the main thread reads the published prefix.
std::atomic<uint32_t> published_channel_count{0};

// Exactly one worker thread publishes. Keep the sequence monotonic across an
// in-process script cleanup, matching the previous bridge contract.
uint64_t next_sequence = 1;

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
	frame.sequence = 0;
	frame.fields.clear();
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

render_channel * find_worker_channel(
	const char * name, size_t length, bool& is_new)
{
	const uint32_t count =
		published_channel_count.load(std::memory_order_relaxed);
	for(uint32_t i = 0; i < count; ++i)
	{
		if(channels[i].matches(name, length))
		{
			is_new = false;
			return &channels[i];
		}
	}
	if(count >= kMaxChannels)
		return nullptr;
	is_new = true;
	channels[count].prepare(name, length);
	return &channels[count];
}

const render_channel * find_render_channel(const char * name, size_t length)
{
	const uint32_t count =
		published_channel_count.load(std::memory_order_acquire);
	for(uint32_t i = 0; i < count; ++i)
		if(channels[i].matches(name, length))
			return &channels[i];
	return nullptr;
}

int publish_render_buffer(lua_State * L)
{
	if(lua_type(L, 1) != LUA_TSTRING || lua_type(L, 2) != LUA_TTABLE)
		return luaL_error(L, "XTLuaPublishRenderBuffer(channel, state) expected");
	size_t channel_length = 0;
	const char * channel_name = lua_tolstring(L, 1, &channel_length);
	if(channel_length == 0 || channel_length > kMaxChannelLength)
		return luaL_error(L, "render buffer channel must be 1..128 bytes");

	bool is_new = false;
	render_channel * channel =
		find_worker_channel(channel_name, channel_length, is_new);
	if(channel == nullptr)
	{
		lua_pushnil(L);
		lua_pushstring(L, "too many render buffer channels");
		return 2;
	}

	render_frame& worker_buffer = channel->worker_buffer();
	const parse_error error = copy_lua_frame(L, worker_buffer);
	if(error != parse_error::none)
	{
		if(is_new)
			channel->reset();
		lua_pushnil(L);
		lua_pushstring(L, error_text(error));
		return 2;
	}

	worker_buffer.sequence = next_sequence++;
	const uint64_t sequence = worker_buffer.sequence;
	channel->publish_worker_buffer();

	if(is_new)
	{
		const uint32_t count =
			published_channel_count.load(std::memory_order_relaxed);
		published_channel_count.store(count + 1, std::memory_order_release);
	}

	lua_pushnumber(L, static_cast<lua_Number>(sequence));
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
	const char * channel_name = lua_tolstring(L, 1, &channel_length);
	const render_channel * channel =
		find_render_channel(channel_name, channel_length);
	if(channel == nullptr)
	{
		lua_pushnil(L);
		return 1;
	}

	const render_frame * render_buffer = channel->render_buffer();
	if(render_buffer == nullptr)
	{
		lua_pushnil(L);
		return 1;
	}

	lua_createtable(L, 0, static_cast<int>(render_buffer->fields.size()));
	for(const auto& field : render_buffer->fields)
	{
		push_render_value(L, field.second);
		lua_setfield(L, -2, field.first.c_str());
	}
	lua_pushnumber(L, static_cast<lua_Number>(render_buffer->sequence));
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

void xtlua_swap_render_buffers()
{
	const uint32_t count =
		published_channel_count.load(std::memory_order_acquire);
	for(uint32_t i = 0; i < count; ++i)
		channels[i].acquire_render_buffer();
}

void xtlua_clear_render_bridge()
{
	// cleanupScripts() calls this only after the worker paused or joined, and
	// the consumer runs on this same main thread.
	published_channel_count.store(0, std::memory_order_release);
	for(render_channel& channel : channels)
		channel.reset();
}
