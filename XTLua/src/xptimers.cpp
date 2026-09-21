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
//      l_timers  ("xlua_*")  -> klassische Variante, wird im SIM-Thread
//                               abgearbeitet, ignoriert den Pausenzustand.
//      x_timers  ("xtlua_*") -> threaded Variante, wird im LUA-Thread
//                               abgearbeitet und haelt bei Pause an.
//
//  SENTINEL-KONVENTION (unveraendert aus dem Original)
//    m_next_fire_time  == -1.0  -> Timer ist NICHT geplant
//    m_repeat_interval == -1.0  -> One-Shot (nach dem Feuern abmelden)
//    Ein delay von -1.0 an run_timer() bedeutet daher "abbestellen".
//
//  THREADING-HINWEIS (T8, bewusst nicht gefixt)
//    Es gibt keinen Mutex. Solange nur der Lua-Thread die xtlua_*-Funktionen
//    und nur der Sim-Thread die xlua_*-Funktionen benutzt, ist das tragbar.
//    ABER: xtlua_timer_cleanup() raeumt BEIDE Listen ab und wird aus dem
//    Sim-Thread gerufen - laeuft dabei parallel Lua-Code, der Timer anlegt
//    oder umplant, ist das ein Race.
//------------------------------------------------------------------------------

#include <cstdio>
#include "xptimers.h"
#include "module.h"
#include "shared_lua_helpers.h"
#include "shared_xpfuncs.h"
#include <stdlib.h>
#include <stdio.h>
//#include <XPLMProcessing.h>
#include <XPLMDataAccess.h>
#include <XPLMUtilities.h>

#include <algorithm>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct xlua_timer {
	xlua_timer *		m_next;
	xlua_timer_f 	m_func;
	void *			m_ref;
	
	double			m_next_fire_time;
	double			m_repeat_interval;	// -1 to stop after 1
	
};

static xlua_timer * x_timers;
static xlua_timer * l_timers;

// XLua 2 timers deliberately do not share the legacy linked lists above. They
// retain their owning lua_State so one module can be torn down without touching
// another module's callbacks.
struct xlua2_timer {
	lua_State *					m_owner;
	std::shared_ptr<notify_cb_t>	m_callback;
	double						m_next_fire_time;
	double						m_repeat_interval;

	xlua2_timer(lua_State * owner, std::shared_ptr<notify_cb_t> callback) :
		m_owner(owner),
		m_callback(std::move(callback)),
		m_next_fire_time(-1.0),
		m_repeat_interval(-1.0)
	{
	}
};

static std::list<xlua2_timer> xlua2_timers;
static const char * const xlua2_timer_callback_key = "TimerCallback";

static int xlua2_debug_proc(lua_State * L)
{
	// XLua 2 module setup stores the traceback stack index here. Accept both a
	// plain number and the userdata form used by Laminar's direct loader.
	lua_getglobal(L, "__debug_proc");
	int debug_proc = 0;
	if(lua_isnumber(L, -1))
	{
		debug_proc = static_cast<int>(lua_tointeger(L, -1));
	}
	else if(lua_type(L, -1) == LUA_TUSERDATA)
	{
		int * value = static_cast<int *>(lua_touserdata(L, -1));
		if(value != NULL)
			debug_proc = *value;
	}
	lua_pop(L, 1);
	return debug_proc;
}

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
		fmt_pcall_stdvars(L, xlua2_debug_proc(L), false, "");
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

	xlua2_timers.emplace_back(L, std::move(callback));
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
	// Callbacks can reschedule timers and can indirectly tear down a complete
	// interpreter. Snapshot only identities, then revalidate every timer before
	// touching it. Newly created timers start on the following flight-loop pass.
	std::vector<std::pair<lua_State *, xlua2_timer *> > due_timers;
	for(xlua2_timer& timer : xlua2_timers)
	{
		module * owner = module::module_from_interp(timer.m_owner);
		if(owner != NULL && owner->is_enabled() &&
			timer.m_next_fire_time >= 0.0 && timer.m_next_fire_time <= now)
			due_timers.emplace_back(timer.m_owner, &timer);
	}

	for(const auto& due : due_timers)
	{
		auto found = xlua2_timer_iterator(due.first, due.second);
		if(found == xlua2_timers.end() ||
			found->m_next_fire_time < 0.0 ||
			found->m_next_fire_time > now)
		{
			continue;
		}

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

xlua_timer * xlua_create_timer(xlua_timer_f func, void * ref)
{
	for(xlua_timer * t = l_timers; t; t = t->m_next)
	if(t->m_func == func && t->m_ref == ref)
	{
		// FIX T6: printf ohne \n klebte an der naechsten Logzeile und war
		// ohne Konsole ohnehin nicht sichtbar.
		std::fprintf(stderr, "xtlua: ERROR: timer already exists.\n");
		return NULL;
	}

	xlua_timer * nt = new xlua_timer;
	nt->m_next = l_timers;
	l_timers = nt;
	nt->m_next_fire_time = -1.0;
	nt->m_repeat_interval = -1.0;
	nt->m_func = func;
	nt->m_ref = ref;
	return nt;
}

void xlua_run_timer(xlua_timer * t, double delay, double repeat)
{
	// FIX T1: xlua_create_timer() liefert bei einem Duplikat NULL, und
	// xlua_is_timer_scheduled() prueft NULL sehr wohl - hier fehlte die Pruefung,
	// also war ein Crash direkt aus Lua heraus ausloesbar.
	if(t == NULL)
		return;

	t->m_repeat_interval = repeat;
	if(delay == -1.0)
		t->m_next_fire_time = delay;		// -1.0 = abbestellen
	else
		t->m_next_fire_time = xlua_get_simulated_time() + delay;
}

int xlua_is_timer_scheduled(xlua_timer * t)
{
	if(t == NULL)
		return 0;
	if(t->m_next_fire_time == -1.0)
		return 0;
	return 1;	
}

double xlua_get_timer_remaining(xlua_timer * t)
{
	if(t == NULL || t->m_next_fire_time == -1.0)
		return -1.0;
	const double remaining = t->m_next_fire_time - xlua_get_simulated_time();
	return remaining > 0.0 ? remaining : 0.0;
}
xlua_timer * xtlua_create_timer(xlua_timer_f func, void * ref)
{
	for(xlua_timer * t = x_timers; t; t = t->m_next)
	if(t->m_func == func && t->m_ref == ref)
	{
		// FIX T6: siehe oben.
		std::fprintf(stderr, "xtlua: ERROR: timer already exists.\n");
		return NULL;
	}

	xlua_timer * nt = new xlua_timer;
	nt->m_next = x_timers;
	x_timers = nt;
	nt->m_next_fire_time = -1.0;
	nt->m_repeat_interval = -1.0;
	nt->m_func = func;
	nt->m_ref = ref;
	return nt;
}

void xtlua_run_timer(xlua_timer * t, double delay, double repeat)
{
	// FIX T1: siehe xlua_run_timer.
	if(t == NULL)
		return;

	t->m_repeat_interval = repeat;
	if(delay == -1.0)
		t->m_next_fire_time = delay;
	else
		t->m_next_fire_time = xlua_get_simulated_time() + delay;
}

int xtlua_is_timer_scheduled(xlua_timer * t)
{
	if(t == NULL)
		return 0;
	if(t->m_next_fire_time == -1.0)
		return 0;
	return 1;	
}

double xtlua_get_timer_remaining(xlua_timer * t)
{
	if(t == NULL || t->m_next_fire_time == -1.0)
		return -1.0;
	const double remaining = t->m_next_fire_time - xlua_get_simulated_time();
	return remaining > 0.0 ? remaining : 0.0;
}

void xtlua_do_timers_for_time(double now,bool isPaused)
{
	// FIX T4: Das Original uebersprang bei Pause nur den Callback, schrieb den
	// Fahrplan aber weiter fort. Ein One-Shot, dessen Zeitpunkt waehrend der
	// Pause erreicht wurde, bekam damit m_next_fire_time = -1.0 und war
	// geloescht, OHNE je gefeuert zu haben. Bei Pause wird die Liste jetzt
	// gar nicht angefasst - der Timer feuert nach dem Fortsetzen nach.
	if(isPaused)
		return;

	// FIX T2: m_next wird VOR dem Callback gesichert. Der Callback ist
	// Lua-Code und kann (ueber einen Reload / xtlua_timer_cleanup) die Liste
	// abraeumen - danach waere t freigegeben und t->m_next ein
	// Use-after-free.
	xlua_timer * t = x_timers;
	while(t)
	{
		xlua_timer * next = t->m_next;

		if(t->m_next_fire_time != -1.0 && t->m_next_fire_time <= now)
		{
			// FIX T3/T5: Fahrplan zuerst, Callback danach - so darf der
			// Callback sich selbst neu einplanen, ohne ueberschrieben zu werden.
			xlua_advance_timer(t, now);
			t->m_func(t->m_ref);
		}

		t = next;
	}
}
void xlua_do_timers_for_time(double now,bool isPaused)
{
	// FIX T7: Diese (Sim-Thread-)Variante ignoriert den Pausenzustand
	// absichtlich - Verhalten wie im Original. Der Parameter bleibt fuer die
	// gemeinsame Signatur erhalten, wird hier aber nicht ausgewertet.
	(void) isPaused;

	// FIX T2/T3/T5: identisch zur xtlua-Variante, siehe Kommentare dort.
	xlua_timer * t = l_timers;
	while(t)
	{
		xlua_timer * next = t->m_next;

		if(t->m_next_fire_time != -1.0 && t->m_next_fire_time <= now)
		{
			xlua_advance_timer(t, now);
			t->m_func(t->m_ref);
		}

		t = next;
	}

	// XLua 2 modules live on the X-Plane thread and use direct SDK bindings.
	// Keep their per-state timers out of the legacy list, but drive them from
	// the same main-thread flight-loop callback.
	xlua2_do_timers_for_time(now);
}
void xtlua_timer_cleanup()
{
	// Hinweis (T8): raeumt bewusst beide Listen ab; zum Thread-Risiko siehe
	// den Kommentar am Dateianfang.
	while(l_timers)
	{
		xlua_timer * k = l_timers;
		l_timers = l_timers->m_next;
		delete k;
	}
	while(x_timers)
	{
		xlua_timer * k = x_timers;
		x_timers = x_timers->m_next;
		delete k;
	}

	xlua2_timer_cleanup();
}
