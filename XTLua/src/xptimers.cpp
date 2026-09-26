//
//  xptimers.cpp
//  xlua
//
//  Created by Benjamin Supnik on 4/13/16.
//
//	Copyright 2016, Laminar Research
//	This source code is licensed under the MIT open source license.
//	See LICENSE.txt for the full terms of the license.

// xTLua
// Modified by Mark Parker on 04/19/2020
//
//
//  xptimers.cpp
//  xlua
//
//  Created by Benjamin Supnik on 4/13/16.
//
//	Copyright 2016, Laminar Research
//	This source code is licensed under the MIT open source license.
//	See LICENSE.txt for the full terms of the license.

// xTLua
// Modified by Mark Parker on 04/19/2020
// Hardened / kommentiert 31/08/2026 - Fixes T1..T8 am Fundort markiert
//
//------------------------------------------------------------------------------
//  ZWECK
//    Zwei unabhaengige, einfach verkettete Timer-Listen:
//
//      l_timers  ("xlua_*")  -> xtlua_main; wird auf dem X-Plane-Thread
//                               abgearbeitet, ignoriert den Pausenzustand.
//      x_timers  ("xtlua_*") -> xtlua_worker; wird auf dem Worker-Thread
//                               abgearbeitet und haelt bei Pause an.
//
//  SENTINEL-KONVENTION (unveraendert aus dem Original)
//    m_next_fire_time  == -1.0  -> Timer ist NICHT geplant
//    m_repeat_interval == -1.0  -> One-Shot (nach dem Feuern abmelden)
//    Ein delay von -1.0 an run_timer() bedeutet daher "abbestellen".
//
//  THREAD-GRENZE
//    Fahrplaene und Listen werden unter timer_mutex kopiert/geaendert,
//    niemals Lua-Callbacks. Die Zeit kommt ausschliesslich aus dem Snapshot.
//    Cleanup erfordert weiterhin einen pausierten/beendeten Worker: Ein Mutex
//    schuetzt Timer-Knoten, nicht die Lebenszeit eines laufenden Lua-States.
//------------------------------------------------------------------------------

#include <cstdio>
#include "xptimers.h"
#include "module.h"
#include "shared_lua_helpers.h"
#include "shared_xpfuncs.h"
#include <stdlib.h>
#include <stdio.h>

#include <algorithm>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <mutex>
#include <cstdint>

struct xlua_timer {
	xlua_timer *		m_next;
	xlua_timer_f 	m_func;
	void *			m_ref;
	
	double			m_next_fire_time;
	double			m_repeat_interval;	// -1 to stop after 1
	std::uint64_t	m_identity;
	std::uint64_t	m_revision;
	
};

static xlua_timer * x_timers;
static xlua_timer * l_timers;
static std::mutex timer_mutex;
static std::uint64_t next_timer_identity = 0;
static bool worker_timer_dispatch_active = false;
static bool main_timer_dispatch_active = false;

// XLua 2 timers deliberately do not share the xtlua linked lists above. They
// retain their owning lua_State so one module can be torn down without touching
// another module's callbacks.
struct xlua2_timer {
	lua_State *					m_owner;
	std::shared_ptr<notify_cb_t>	m_callback;
	double						m_next_fire_time;
	double						m_repeat_interval;
	std::uint64_t				m_identity;
	std::uint64_t				m_revision = 0;

	xlua2_timer(lua_State * owner, std::shared_ptr<notify_cb_t> callback,
		std::uint64_t identity) :
		m_owner(owner),
		m_callback(std::move(callback)),
		m_next_fire_time(-1.0),
		m_repeat_interval(-1.0),
		m_identity(identity)
	{
	}
};

static std::list<xlua2_timer> xlua2_timers;
static std::uint64_t next_xlua2_timer_identity = 0;
static bool xlua2_timer_dispatch_active = false;
static const char * const xlua2_timer_callback_key = "TimerCallback";

static int xlua2_absolute_stack_index(lua_State * L, int index)
{
	return index > 0 ? index : lua_gettop(L) + index + 1;
}

static bool xlua2_callback_matches(
	lua_State * L,
	const xlua2_timer& timer,
	int function_index)
{
	if(timer.m_owner != L || !timer.m_callback)
		return false;

	const auto callback = timer.m_callback->callbacks.find(
		xlua2_timer_callback_key);
	if(callback == timer.m_callback->callbacks.end() || callback->second <= 0)
		return false;

	const int absolute_index = xlua2_absolute_stack_index(L, function_index);
	lua_rawgeti(L, LUA_REGISTRYINDEX, callback->second);
	const bool matches = lua_rawequal(L, -1, absolute_index) != 0;
	lua_pop(L, 1);
	return matches;
}

static std::list<xlua2_timer>::iterator xlua2_timer_iterator(
	lua_State * L,
	xlua2_timer * timer)
{
	return std::find_if(
		xlua2_timers.begin(),
		xlua2_timers.end(),
		[L, timer](xlua2_timer& candidate) {
			return &candidate == timer && candidate.m_owner == L;
		});
}

static void xlua2_invoke_timer(const std::shared_ptr<notify_cb_t>& callback)
{
	if(!callback)
		return;

	lua_State * L = setup_lua_callback(
		callback.get(),
		xlua2_timer_callback_key);
	if(L != NULL)
	{
		// The loader's saved index is not necessarily valid on a reentrant C
		// callback stack. Do not dereference script-replaceable debug userdata.
		fmt_pcall_stdvars(L, 0, false, ""); // helper owns the local traceback
	}
}

xlua2_timer * xlua2_create_timer(lua_State * L, int function_index)
{
	if(L == NULL)
		return NULL;

	luaL_checktype(L, function_index, LUA_TFUNCTION);
	if(xlua2_find_timer(L, function_index) != NULL)
		return NULL;

	auto callback = std::make_shared<notify_cb_t>(
		L,
		notify_cb_t::kNeverPersist);
	if(!wrap_next_lua_func(
		callback,
		function_index,
		false,
		xlua2_timer_callback_key))
	{
		return NULL;
	}

	xlua2_timers.emplace_back(L, std::move(callback), ++next_xlua2_timer_identity);
	return &xlua2_timers.back();
}

xlua2_timer * xlua2_find_timer(lua_State * L, int function_index)
{
	if(L == NULL)
		return NULL;

	luaL_checktype(L, function_index, LUA_TFUNCTION);
	for(xlua2_timer& timer : xlua2_timers)
	{
		if(xlua2_callback_matches(L, timer, function_index))
			return &timer;
	}
	return NULL;
}

bool xlua2_run_timer(
	lua_State * L,
	xlua2_timer * timer,
	double delay,
	double repeat)
{
	const auto found = xlua2_timer_iterator(L, timer);
	if(found == xlua2_timers.end())
		return false;
	++found->m_revision;

	if(delay < 0.0)
	{
		found->m_next_fire_time = -1.0;
		found->m_repeat_interval = -1.0;
	}
	else
	{
		found->m_next_fire_time = xlua_get_simulated_time() + delay;
		found->m_repeat_interval = repeat > 0.0 ? repeat : -1.0;
	}
	return true;
}

bool xlua2_is_timer_scheduled(lua_State * L, xlua2_timer * timer)
{
	const auto found = xlua2_timer_iterator(L, timer);
	return found != xlua2_timers.end() && found->m_next_fire_time >= 0.0;
}

double xlua2_get_timer_remaining(lua_State * L, xlua2_timer * timer)
{
	const auto found = xlua2_timer_iterator(L, timer);
	if(found == xlua2_timers.end() || found->m_next_fire_time < 0.0)
		return -1.0;

	const double remaining =
		found->m_next_fire_time - xlua_get_simulated_time();
	return remaining > 0.0 ? remaining : 0.0;
}

void xlua2_do_timers_for_time(double now)
{
	if(xlua2_timer_dispatch_active)
		return;
	xlua2_timer_dispatch_active = true;
	struct reset_dispatch {
		~reset_dispatch() { xlua2_timer_dispatch_active = false; }
	} reset;
	// Callbacks can reschedule timers and can indirectly tear down a complete
	// interpreter. Identity/revision guards also reject allocator address reuse
	// or an existing timer rearmed by an earlier callback in this dispatch.
	struct due_timer {
		lua_State * owner;
		xlua2_timer * timer;
		std::uint64_t identity;
		std::uint64_t revision;
	};
	std::vector<due_timer> due_timers;
	for(xlua2_timer& timer : xlua2_timers)
	{
		module * owner = module::module_from_interp(timer.m_owner);
		if(owner != NULL && owner->is_enabled() &&
			timer.m_next_fire_time >= 0.0 && timer.m_next_fire_time <= now)
			due_timers.push_back({timer.m_owner, &timer, timer.m_identity, timer.m_revision});
	}

	for(const auto& due : due_timers)
	{
		auto found = xlua2_timer_iterator(due.owner, due.timer);
		if(found == xlua2_timers.end() ||
			found->m_identity != due.identity || found->m_revision != due.revision ||
			found->m_next_fire_time < 0.0 ||
			found->m_next_fire_time > now)
		{
			continue;
		}
		module * owner = module::module_from_interp(found->m_owner);
		if(owner == NULL || !owner->is_enabled())
			continue;

		// Set the default next state before entering Lua. A callback that calls
		// XLuaRunTimer for itself therefore wins and keeps its new schedule.
		if(found->m_repeat_interval > 0.0)
		{
			found->m_next_fire_time += found->m_repeat_interval;
			if(found->m_next_fire_time <= now)
				found->m_next_fire_time = now + found->m_repeat_interval;
		}
		else
		{
			found->m_next_fire_time = -1.0;
			found->m_repeat_interval = -1.0;
		}

		const std::shared_ptr<notify_cb_t> callback = found->m_callback;
		xlua2_invoke_timer(callback);
	}
}

bool xlua2_destroy_timer(lua_State * L, xlua2_timer * timer)
{
	const auto found = xlua2_timer_iterator(L, timer);
	if(found == xlua2_timers.end())
		return false;
	xlua2_timers.erase(found);
	return true;
}

void xlua_remove_timers_for_state(lua_State * L)
{
	if(L == NULL)
		return;

	for(auto timer = xlua2_timers.begin(); timer != xlua2_timers.end(); )
	{
		if(timer->m_owner == L)
			timer = xlua2_timers.erase(timer);
		else
			++timer;
	}
}

void xlua2_timer_cleanup()
{
	// Normal teardown removes every state while it is still open. Nulling an
	// unexpected survivor avoids dereferencing a closed lua_State in the
	// notify_cb_t destructor during final process cleanup.
	for(xlua2_timer& timer : xlua2_timers)
	{
		if(timer.m_callback)
			timer.m_callback->L = NULL;
	}
	xlua2_timers.clear();
}

//------------------------------------------------------------------------------
// Gemeinsame Hilfsroutine fuer den Fahrplan EINES faelligen Timers.
//
// FIX T3 (Kern des Problems): Im Original wurde erst der Callback gerufen und
// DANACH m_next_fire_time neu berechnet. Plant sich ein One-Shot in seinem
// eigenen Callback neu ein - das uebliche Muster
//
//     function tick()  ...  run_after_time(tick, 5)  end
//
// - dann sah der Code hinterher m_repeat_interval == -1.0 und setzte
// m_next_fire_time auf -1.0 zurueck. Der Timer war damit stillschweigend
// geloescht, obwohl Lua ihn gerade neu bestellt hatte.
// Loesung: Fahrplan VOR dem Callback festschreiben. Aendert der Callback
// etwas, gewinnt er.
//
// FIX T5: m_next_fire_time += interval kann nach einem Freeze (Ladevorgang,
// Frame-Hitch) weit in der Vergangenheit liegen. Das Original arbeitete den
// Rueckstau dann ueber viele Frames Tick fuer Tick ab. Liegen wir mehr als ein
// Intervall zurueck, wird stattdessen auf now + interval aufgesetzt und die
// verpassten Ticks werden verworfen.
//------------------------------------------------------------------------------
static void xlua_advance_timer(xlua_timer * t, double now)
{
	if(t->m_repeat_interval == -1.0)
	{
		t->m_next_fire_time = -1.0;			// One-Shot: abmelden
		return;
	}

	t->m_next_fire_time += t->m_repeat_interval;

	// Nur bei positivem Intervall aufholen. Ein Intervall von 0.0 (oder
	// negativ) soll wie im Original weiter jeden Frame feuern - das wird hier
	// absichtlich nicht "korrigiert".
	if(t->m_repeat_interval > 0.0 && t->m_next_fire_time <= now)
		t->m_next_fire_time = now + t->m_repeat_interval;
}

static xlua_timer * find_timer(xlua_timer * head, xlua_timer * wanted)
{
	for(xlua_timer * timer = head; timer; timer = timer->m_next)
		if(timer == wanted)
			return timer;
	return NULL;
}

static xlua_timer * create_timer(xlua_timer *& head, xlua_timer_f func, void * ref)
{
	std::lock_guard<std::mutex> lock(timer_mutex);
	for(xlua_timer * timer = head; timer; timer = timer->m_next)
		if(timer->m_func == func && timer->m_ref == ref)
		{
			// The Lua binding reports this failure with its owning script name.
			return NULL;
		}
	xlua_timer * timer = new xlua_timer;
	timer->m_next = head;
	timer->m_next_fire_time = -1.0;
	timer->m_repeat_interval = -1.0;
	timer->m_func = func;
	timer->m_ref = ref;
	timer->m_identity = ++next_timer_identity;
	timer->m_revision = 0;
	head = timer;
	return timer;
}

static void run_timer(xlua_timer *& head, xlua_timer * timer, double delay, double repeat)
{
	const double now = xlua_get_simulated_time();
	std::lock_guard<std::mutex> lock(timer_mutex);
	if(!find_timer(head, timer))
		return;
	++timer->m_revision;
	timer->m_repeat_interval = repeat;
	timer->m_next_fire_time = delay == -1.0 ? -1.0 : now + delay;
}

static int timer_scheduled(xlua_timer *& head, xlua_timer * timer)
{
	std::lock_guard<std::mutex> lock(timer_mutex);
	return find_timer(head, timer) && timer->m_next_fire_time != -1.0;
}

static double timer_remaining(xlua_timer *& head, xlua_timer * timer)
{
	const double now = xlua_get_simulated_time();
	std::lock_guard<std::mutex> lock(timer_mutex);
	if(!find_timer(head, timer) || timer->m_next_fire_time == -1.0)
		return -1.0;
	return (std::max)(0.0, timer->m_next_fire_time - now);
}

xlua_timer * xlua_create_timer(xlua_timer_f func, void * ref)
{
	return create_timer(l_timers, func, ref);
}
xlua_timer * xtlua_create_timer(xlua_timer_f func, void * ref)
{
	return create_timer(x_timers, func, ref);
}
void xlua_run_timer(xlua_timer * timer, double delay, double repeat)
{
	run_timer(l_timers, timer, delay, repeat);
}
void xtlua_run_timer(xlua_timer * timer, double delay, double repeat)
{
	run_timer(x_timers, timer, delay, repeat);
}
int xlua_is_timer_scheduled(xlua_timer * timer)
{
	return timer_scheduled(l_timers, timer);
}
int xtlua_is_timer_scheduled(xlua_timer * timer)
{
	return timer_scheduled(x_timers, timer);
}
double xlua_get_timer_remaining(xlua_timer * timer)
{
	return timer_remaining(l_timers, timer);
}
double xtlua_get_timer_remaining(xlua_timer * timer)
{
	return timer_remaining(x_timers, timer);
}

struct timer_dispatch_scope {
	bool& active;
	bool entered;
	explicit timer_dispatch_scope(bool& dispatch_active) : active(dispatch_active)
	{
		std::lock_guard<std::mutex> lock(timer_mutex);
		entered = !active;
		if(entered)
			active = true;
	}
	~timer_dispatch_scope()
	{
		if(entered)
		{
			std::lock_guard<std::mutex> lock(timer_mutex);
			active = false;
		}
	}
};

static void dispatch_timers(xlua_timer *& head, double now)
{
	struct due_timer {
		xlua_timer * timer;
		std::uint64_t identity;
		std::uint64_t revision;
	};
	std::vector<due_timer> due;
	{
		std::lock_guard<std::mutex> lock(timer_mutex);
		for(xlua_timer * timer = head; timer; timer = timer->m_next)
			if(timer->m_next_fire_time != -1.0 && timer->m_next_fire_time <= now)
				due.push_back({timer, timer->m_identity, timer->m_revision});
	}
	for(const due_timer& item : due)
	{
		xlua_timer_f callback = NULL;
		void * ref = NULL;
		{
			std::lock_guard<std::mutex> lock(timer_mutex);
			xlua_timer * timer = find_timer(head, item.timer);
			// An earlier callback may cancel/rearm this timer, remove the whole
			// list, or create a new timer at a recycled address.
			if(!timer || timer->m_identity != item.identity ||
				timer->m_revision != item.revision ||
				timer->m_next_fire_time == -1.0 || timer->m_next_fire_time > now)
				continue;
			xlua_advance_timer(timer, now);
			callback = timer->m_func;
			ref = timer->m_ref;
		}
		// No timer/list lock crosses Lua. A self-reschedule wins; newly created
		// or rearmed timers are considered in the next dispatch, not this batch.
		if(callback)
			callback(ref);
	}
}

void xtlua_do_timers_for_time(double now, bool isPaused)
{
	if(isPaused)
		return;
	timer_dispatch_scope dispatch(worker_timer_dispatch_active);
	if(dispatch.entered)
		dispatch_timers(x_timers, now);
}

void xlua_do_timers_for_time(double now, bool isPaused)
{
	(void)isPaused; // xtlua_main preserves its existing pause-independent mode.
	timer_dispatch_scope dispatch(main_timer_dispatch_active);
	if(!dispatch.entered)
		return;
	dispatch_timers(l_timers, now);
	xlua2_do_timers_for_time(now);
}

bool xlua_is_main_timer_dispatch_active()
{
	std::lock_guard<std::mutex> lock(timer_mutex);
	return main_timer_dispatch_active || xlua2_timer_dispatch_active;
}

void xtlua_timer_cleanup()
{
	// Lifecycle guarantees no worker callback is running, and module shutdown
	// guards forbid finalizers from creating or using timers after retirement.
	xlua_timer * old_main;
	xlua_timer * old_worker;
	{
		std::lock_guard<std::mutex> lock(timer_mutex);
		old_main = l_timers;
		old_worker = x_timers;
		l_timers = NULL;
		x_timers = NULL;
	}
	while(old_main)
	{
		xlua_timer * next = old_main->m_next;
		delete old_main;
		old_main = next;
	}
	while(old_worker)
	{
		xlua_timer * next = old_worker->m_next;
		delete old_worker;
		old_worker = next;
	}
	xlua2_timer_cleanup();
}
