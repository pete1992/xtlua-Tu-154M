//	Copyright 2016, Laminar Research
//	This source code is licensed under the MIT open source license.
//	See LICENSE.txt for the full terms of the license.
// xTLua
// Modified by Mark Parker on 04/19/2020
// Modifikation by Peter Schwake on 24/08/2026

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <vector>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#define XTVERSION "2.4.9"
#include <thread>
#include <mutex>
#include <condition_variable>
#ifndef XPLM200
#define XPLM200
#endif
#ifndef XPLM210
#define XPLM210
#endif
#ifndef XPLM300
#define XPLM300
#endif
#ifndef XPLM301
#define XPLM301
#endif
#ifndef XPLM302
#define XPLM302
#endif
#ifndef XPLM303
#define XPLM303
#endif
#ifndef XPLM400
#define XPLM400
#endif
#ifndef XPLM410
#define XPLM410
#endif
#ifndef XPLM420
#define XPLM420
#endif
#ifndef XPLM430
#define XPLM430
#endif
#ifndef XPLM440
#define XPLM440
#endif
#ifndef XPLMPG1
#define XPLMPG1
#endif
#include <XPLMPlugin.h>
#include <XPLMDataAccess.h>
#include <XPLMUtilities.h>
#include <XPLMProcessing.h>
#include "module.h"
#include "xpdatarefs.h"
#include "xpcommands.h"
#include "xptimers.h"
#include "shared_xpfuncs.h"
#include "xlua2_host.h"
#include "xtlua_render_bridge.h"
#include "SerialWidget.h"
using std::vector;

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#if !MOBILE
//static void *			g_alloc = NULL;
#endif
static vector<module *>xtlua_worker_modules; // xtlua_worker: asynchronous Lua; buffered XPLM access.
static vector<module *>xtlua_main_modules;   // xtlua_main: X-Plane thread; direct XLua-compatible access.
static vector<module *>xlua2_main_modules;   // xlua2_main: X-Plane thread; direct SDK 4.4 bindings.
static XPLMFlightLoopID	g_pre_loop = NULL;
static XPLMFlightLoopID	g_post_loop = NULL;
static int				g_is_acf_inited = 0;
XPLMDataRef				g_replay_active = NULL;
XPLMDataRef				g_sim_period = NULL;
int myID=0;
struct lua_alloc_request_t {
            void *	ud;
            void *	ptr;
            size_t	osize;
            size_t	nsize;
};

#define		ALLOC_OPEN		0x00A110C1
#define		ALLOC_REALLOC	0x00A110C2
#define		ALLOC_CLOSE		0x00A110C3
#define		ALLOC_LOCK		0x00A110C4
#define		ALLOC_UNLOCK	0x00A110C5

std::atomic_bool ready{false};
std::atomic_bool loadedModules{false};
std::atomic_bool liveThread{false};
std::atomic_bool run{true};
std::atomic_bool active{false};
std::atomic_bool dirtyXTScripts{false};
std::atomic_bool sleeping{false};
// Admission to both script loading and Lua execution uses this mutex. A
// pause request prevents new work and waits for the admitted cycle to leave;
// the old sleeping flag alone could acknowledge a stale idle state.
static std::mutex g_worker_state_mutex;
static std::condition_variable g_worker_state_changed;
static bool g_worker_pause_requested = false;
static bool g_worker_busy = false;

static void pauseWorker()
{
    std::unique_lock<std::mutex> lock(g_worker_state_mutex);
    active = false;
    g_worker_pause_requested = true;
    g_worker_state_changed.notify_all();
    g_worker_state_changed.wait(lock, [] { return !g_worker_busy; });
}

static void resumeWorker(bool enable)
{
    {
        std::lock_guard<std::mutex> lock(g_worker_state_mutex);
        active = enable;
        g_worker_pause_requested = false;
    }
    g_worker_state_changed.notify_all();
}
static std::atomic_bool g_xlua2_reload_requested{false};
static std::atomic_bool g_script_reload_requested{false};
static bool g_script_cleanup_active = false;
static int performScriptReload();
static bool g_xlua2_reload_on_flight_change = false;
static bool g_plugin_enabled = false;
static bool g_host_transition_active = false; // Main thread, including SDK reentry.
struct HostTransitionScope {
    HostTransitionScope() { g_host_transition_active = true; }
    ~HostTransitionScope() { g_host_transition_active = false; }
};
static std::unordered_map<int, string> g_xlua2_event_param_types;

void xlua_register_event(int event_id, const char * event_param_type)
{
    g_xlua2_event_param_types[event_id] = event_param_type ? event_param_type : "";
}

string xlua2_event_param_type(int event_id)
{
    const auto found = g_xlua2_event_param_types.find(event_id);
    return found == g_xlua2_event_param_types.end() ? string() : found->second;
}

void xlua2_host_reload_on_flight_change()
{
    g_xlua2_reload_on_flight_change = true;
}

static float xlua_pre_timer_master_cb(
                                   float                /*inElapsedSinceLastCall*/,
                                   float                /*inElapsedTimeSinceLastFlightLoop*/,
                                   int                  /*inCounter*/,
                                   void *               /*inRefcon*/)
{
    xtlua_flush_log_queue();
    // SDK accessors/commands can re-enter host callbacks. Never reload or
    // recursively dispatch a new bridge batch on an unfinished SDK stack.
    if(xtlua_is_sdk_dispatch_active())
        return -1;
    if(g_xlua2_reload_requested.exchange(false))
    {
        XPLMReloadThisPlugin(false);
        return 0.0f;
    }

    if(g_script_reload_requested.exchange(false))
    {
        if(!ready || !loadedModules)
            XPLMReloadThisPlugin(false);
        else
            performScriptReload();
        return -1;
    }

    // Freeze one coherent render_buffer per channel for this X-Plane frame.
    // Lua draw callbacks only copy from these main-thread-owned slots.
    xtlua_swap_render_buffers();

    xlua_do_timers_for_time(xlua_get_simulated_time(),xlua_ispaused());

if(loadedModules&&xtlua_dref_resolveDREFQueue()==0&&!ready){
        {
            std::lock_guard<std::mutex> lock(g_worker_state_mutex);
            ready=true;
        }
        g_worker_state_changed.notify_all();
    }
    if(ready)
        xtlua_dref_preUpdate();

    if(XPLMGetDatai(g_replay_active) == 0 &&
       XPLMGetDataf(g_sim_period) > 0.0f)
    {
        for(module * m : xlua2_main_modules)
            m->pre_physics();
    }
    return -1;
}

static float xlua_post_timer_master_cb(
                                   float                /*inElapsedSinceLastCall*/,
                                   float                /*inElapsedTimeSinceLastFlightLoop*/,
                                   int                  /*inCounter*/,
                                   void *               /*inRefcon*/)
{
    if(XPLMGetDatai(g_replay_active) == 0)
    {
        if(XPLMGetDataf(g_sim_period) > 0.0f)
        {
            for(vector<module *>::iterator m = xtlua_main_modules.begin(); m != xtlua_main_modules.end(); ++m)
                (*m)->post_physics();
            for(module * m : xlua2_main_modules)
                m->post_physics();
        }
    }
    else
    {
        for(module * m : xlua2_main_modules)
            m->post_replay();
    }
    if(ready)
        xtlua_dref_postUpdate();
    xtlua_flush_log_queue();
    {
        std::lock_guard<std::mutex> lock(g_worker_state_mutex);
        liveThread=true;
    }
    g_worker_state_changed.notify_all();
    return -1;
}
std::vector<string> script_paths;
std::vector<string> mod_paths;
static std::vector<string> xlua2_script_paths;
static std::vector<string> xlua2_mod_paths;
string init_script_path;
static string xlua2_init_script_path;
string plugin_base_path;

void registerFlightLoop(){
    XPLMCreateFlightLoop_t pre = { 0 };
    XPLMCreateFlightLoop_t post = { 0 };
    pre.structSize = sizeof(pre);
    post.structSize = sizeof(post);
    pre.phase = xplm_FlightLoop_Phase_BeforeFlightModel;
    post.phase = xplm_FlightLoop_Phase_AfterFlightModel;
    pre.callbackFunc = xlua_pre_timer_master_cb;
    post.callbackFunc = xlua_post_timer_master_cb;
    g_pre_loop = XPLMCreateFlightLoop(&pre);
    g_post_loop = XPLMCreateFlightLoop(&post);
    XPLMScheduleFlightLoop(g_pre_loop, -1, 0);
    XPLMScheduleFlightLoop(g_post_loop, -1, 0);
}

static void findXTScripts(){
    string scripts_dir_path(plugin_base_path);
    // Discover xtlua_worker scripts and xlua2_main scripts on the X-Plane thread.
    init_script_path=plugin_base_path;
    init_script_path += "init.lua";
    xlua2_init_script_path=plugin_base_path;
    xlua2_init_script_path += "init_v2.lua";
    scripts_dir_path=plugin_base_path;
    scripts_dir_path += "scripts";
    int offset = 0;
    int mf, fcount;
    while(1)
    {
        char fname_buf[2048];
        char * fptr;
        XPLMGetDirectoryContents(
                                scripts_dir_path.c_str(),
                                offset,
                                fname_buf,
                                sizeof(fname_buf),
                                &fptr,
                                1,
                                &mf,
                                &fcount);
        if(fcount == 0)
            break;

        if(strcmp(fptr, ".DS_Store") != 0)
        {
            string mod_path(scripts_dir_path);
            mod_path += "/";
            mod_path += fptr;
            mod_path += "/";
            string script_path(mod_path);
            script_path += fptr;
            script_path += ".lua";
            const module_script_kind script_kind =
                module::classify_script(script_path.c_str());
            if(script_kind == module_script_kind::xlua2)
            {
                xlua2_mod_paths.push_back(mod_path);
                xlua2_script_paths.push_back(script_path);
            }
            else if(script_kind == module_script_kind::xtlua)
            {
                mod_paths.push_back(mod_path);
                script_paths.push_back(script_path);
            }
            else
            {
                log_message(nullptr, "%s: unsupported XLua marker; use exact first line --[[ XLua 2.0 ]]\n",
                    script_path.c_str());
            }
        }
        ++offset;
        if(offset == mf)
            break;
    }

    dirtyXTScripts=true;
}
static void loadXTScripts(){
    for(int i=0;i<static_cast<int>(script_paths.size());i++)
    {
#if !MOBILE
            xtlua_worker_modules.push_back(new module(
                            mod_paths[i].c_str(),
                            init_script_path.c_str(),
                            script_paths[i].c_str(),
                            module_runtime::xtlua_worker));
#else
            xtlua_worker_modules.push_back(new module(
                mod_paths[i].c_str(),
                init_script_path.c_str(),
                script_paths[i].c_str(),
                lj_alloc_f,
                NULL));
#endif
    }
    dirtyXTScripts=false;
}

static bool loadXLua2Scripts()
{
    for(size_t i = 0; i < xlua2_script_paths.size(); ++i)
    {
        module * candidate = new module(
            xlua2_mod_paths[i].c_str(),
            xlua2_init_script_path.c_str(),
            xlua2_script_paths[i].c_str(),
            module_runtime::xlua2_main);
        if(candidate->is_valid() && candidate->xplugin_start())
        {
            xlua2_main_modules.push_back(candidate);
        }
        else
        {
            log_message(nullptr, "xlua2_main: %s: load/XPluginStart failed; refusing host startup\n",
                candidate->get_script_path().c_str());
            delete candidate;
            return false;
        }
    }
    return true;
}

static void loadXPScripts(){
    string scripts_dir_path(plugin_base_path);
    init_script_path=plugin_base_path;
    init_script_path += "init/init.lua";
    scripts_dir_path += "init/scripts";
    int offset = 0;
    int mf, fcount;
    while(1)
    {
        char fname_buf[2048];
        char * fptr;
        XPLMGetDirectoryContents(
                                scripts_dir_path.c_str(),
                                offset,
                                fname_buf,
                                sizeof(fname_buf),
                                &fptr,
                                1,
                                &mf,
                                &fcount);
        if(fcount == 0)
            break;

        if(strcmp(fptr, ".DS_Store") != 0)
        {
            string mod_path(scripts_dir_path);
            mod_path += "/";
            mod_path += fptr;
            mod_path += "/";
            string script_path(mod_path);
            script_path += fptr;
            script_path += ".lua";
            xtlua_main_modules.push_back(new module(
                            mod_path.c_str(),
                            init_script_path.c_str(),
                            script_path.c_str(),
                            module_runtime::xtlua_main));
        }
        ++offset;
        if(offset == mf)
            break;
    }
}

static void cleanupScripts(bool keepFlightLoops = false){
    // In-process reload is serviced only at the frame boundary. SDK plugin
    // Stop is delivered after the current callback returns (SDK contract).
    assert(!xtlua_is_sdk_dispatch_active());
    assert(!g_script_cleanup_active);
    g_script_cleanup_active = true;
    if(!keepFlightLoops)
    {
        if(g_pre_loop!=NULL)
            XPLMDestroyFlightLoop(g_pre_loop);
        if(g_post_loop!=NULL)
            XPLMDestroyFlightLoop(g_post_loop);
        g_pre_loop = NULL;
        g_post_loop = NULL;
    }
    if(g_is_acf_inited)
    {
        for(module * m : xtlua_main_modules)
            m->acf_unload();
        for(module * m : xlua2_main_modules)
            m->acf_unload();
    }
    g_is_acf_inited = 0;

    // The worker has stopped or paused before cleanupScripts() is entered.
    // Retire its immutable display snapshots before main-thread draw owners
    // and module Lua states are torn down.
    xtlua_clear_render_bridge();
	serialWindow.cleanup(); // Shared native window is owned by the main thread.

    // xlua2_main shutdown hooks run while XPLM is still available. Each module
    // then unregisters its direct command handlers and timers before closing
    // its Lua state.
    for(module * m : xlua2_main_modules)
        delete m;
    xlua2_main_modules.clear();

    // Reject classic binding calls from __gc before their handle storage is
    // retired. Finalizers may still log; logging owns text independently.
    for(module * m : xtlua_main_modules)
        m->prepare_shutdown();
    for(module * m : xtlua_worker_modules)
        m->prepare_shutdown();
    xtlua_dref_cleanup();
    xtlua_cmd_cleanup();
    xtlua_timer_cleanup();
    for(vector<module *>::iterator m = xtlua_main_modules.begin(); m != xtlua_main_modules.end(); ++m)
        delete (*m);
    xtlua_main_modules.clear();
    for(vector<module *>::iterator m = xtlua_worker_modules.begin(); m != xtlua_worker_modules.end(); ++m)
        delete (*m);
    xtlua_worker_modules.clear();
    // lua_close() may run worker-state __gc finalizers. Discard any frame a
    // finalizer published after the first clear before a reload can continue.
    xtlua_clear_render_bridge();
    script_paths.clear();
    mod_paths.clear();
    xlua2_script_paths.clear();
    xlua2_mod_paths.clear();
    // Includes queued aircraft_unload and lua_close/__gc diagnostics.
    while(xtlua_flush_log_queue() != 0) {}
    g_script_cleanup_active = false;
}

int reloadScripts(XPLMCommandRef c, XPLMCommandPhase phase, void * ref){
    if(phase == xplm_CommandBegin && !g_script_cleanup_active)
        g_script_reload_requested = true;
    return 0;
}

static int performScriptReload(){
    {
        // Generic SDK resources created by XLua 2 (windows, flight loops,
        // accessors, map hooks, ...) cannot all be reconstructed safely by
        // the xtlua in-process script reload. Let SDK 4.4 reload the entire
        // plug-in so XPLM first removes every resource owned by this plug-in.
        if(!xlua2_main_modules.empty())
        {
            g_xlua2_reload_requested=true;
            return 0;
        }

        // Pause xtlua_worker before the X-Plane thread reloads scripts.
        pauseWorker();
        loadedModules=false;
        ready=false;
        // Keep the currently executing host flight loop alive. Its next tick
        // sees the newly loaded state, never a half-destroyed callback owner.
        cleanupScripts(true);
        loadXPScripts();
        xlua_relink_all_drefs();
        findXTScripts();
        if(!loadXLua2Scripts())
        {
            // This can only be reached when xlua2_main was added since the last
            // discovery. We are already in an X-Plane-thread frame callback;
            // ask the SDK to retire the failed load after this callback returns.
            XPLMReloadThisPlugin(false);
            return 0;
        }

        // Admit only loading; Lua cycles still require ready and active.
        resumeWorker(false);
        {
            std::unique_lock<std::mutex> lock(g_worker_state_mutex);
            g_worker_state_changed.wait(lock, [] {
                return (!dirtyXTScripts && !g_worker_busy) || !run;
            });
        }
        if(g_plugin_enabled)
        {
            size_t enabled_count = 0;
            for(module * m : xlua2_main_modules)
            {
                if(!m->xplugin_enable())
                {
                    while(enabled_count > 0)
                        xlua2_main_modules[--enabled_count]->xplugin_disable();
                    XPLMReloadThisPlugin(false);
                    return 0;
                }
                ++enabled_count;
            }
        }
        //init scripts
        xlua_add_callout("aircraft_load");
        xlua_add_callout("flight_start");
        xlua_setLoadStatus(1);
        resumeWorker(g_plugin_enabled);
    }

    return 0;
}
void do_during_physics(){
    for(;;){
        bool load_scripts = false;
        {
            std::unique_lock<std::mutex> lock(g_worker_state_mutex);
            g_worker_busy = false;
            sleeping = true;
            g_worker_state_changed.notify_all();
            g_worker_state_changed.wait(lock, [] {
                return !run || (!g_worker_pause_requested && liveThread &&
                    (dirtyXTScripts || (active && ready)));
            });
            if(!run)
                break;
            g_worker_busy = true;
            sleeping = false;
            load_scripts = dirtyXTScripts;
        }
        const auto start = std::chrono::steady_clock::now();
        try{
            if(load_scripts){
                loadXTScripts();
                loadedModules=true;
            }
            else{
                std::vector<XTCmd*> runItems=get_runQueue();
                struct command_batch_owner {
                    std::vector<XTCmd*>& items;
                    ~command_batch_owner() {
                        for(XTCmd * item : items)
                            delete item;
                    }
                } command_owner{runItems};
                for(XTCmd* item:runItems){
                    item->runFunc(item->xluaref, item->phase, item->duration, item->m_func_ref);
                }
                xtlua_do_timers_for_time(xlua_get_simulated_time(),xlua_ispaused());
                std::vector<string> msgItems=get_runMessages();

                for(string item:msgItems){

                        for(vector<module *>::iterator m = xtlua_worker_modules.begin(); m != xtlua_worker_modules.end(); ++m)
                            (*m)->do_callout(item.c_str());
                    }
                if(!xlua_ispaused()){
                    for(vector<module *>::iterator m = xtlua_worker_modules.begin(); m != xtlua_worker_modules.end(); ++m){
                        int ret=(*m)->pre_physics();
                        if(ret!=0)
                            active=false;
                    }

                    for(vector<module *>::iterator m = xtlua_worker_modules.begin(); m != xtlua_worker_modules.end(); ++m)
                    {
                        int ret=(*m)->post_physics();
                        if(ret!=0)
                            active=false;
                    }
                }

                xtlua_localNavData();
            }
        }
        catch(...){
            log_message(nullptr, "xtlua_worker exception; runtime paused\n");
            active=false;
            // Do not spin retrying an incomplete load. An explicit reload
            // uses SDK plugin reload while the bridge is not ready.
            if(load_scripts)
                dirtyXTScripts=false;
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const auto min_frame = std::chrono::milliseconds(20);
        std::unique_lock<std::mutex> lock(g_worker_state_mutex);
        g_worker_busy = false;
        sleeping = true;
        g_worker_state_changed.notify_all();
        if(!load_scripts && elapsed < min_frame)
            g_worker_state_changed.wait_for(lock, min_frame - elapsed, [] {
                return !run || g_worker_pause_requested;
            });
    }

    if(g_is_acf_inited)
    {
        for(vector<module *>::iterator m = xtlua_worker_modules.begin(); m != xtlua_worker_modules.end(); ++m)
            (*m)->acf_unload();
    }
    sleeping=true;
}
std::vector<std::thread> threads;
int XTLuaXPluginStart(char * outSig)
{
    xtlua_log_set_main_thread();
    ready=false;
    loadedModules=false;
    liveThread=false;
    run=true;
    active=false;
    dirtyXTScripts=false;
    sleeping=false;
    g_worker_pause_requested=false;
    g_worker_busy=false;
    g_plugin_enabled=false;
    g_host_transition_active=false;
    g_xlua2_reload_requested=false;
    g_script_reload_requested=false;
    g_script_cleanup_active=false;
    g_xlua2_reload_on_flight_change=false;
    g_xlua2_event_param_types.clear();
    g_is_acf_inited = 0;
    g_replay_active = XPLMFindDataRef("sim/time/is_in_replay");
    g_sim_period = XPLMFindDataRef("sim/operation/misc/frame_rate_period");
    myID=XPLMGetMyID();
    registerFlightLoop();
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);
    char path_to_me_c[2048];
    XPLMGetPluginInfo(XPLMGetMyID(), NULL, path_to_me_c, NULL, NULL);

    // Plugin base path: pop off two dirs from the plugin name to get the base path.
    plugin_base_path=string(path_to_me_c);
    string::size_type lp = plugin_base_path.find_last_of("/\\");
    plugin_base_path.erase(lp);
    lp = plugin_base_path.find_last_of("/\\");
    plugin_base_path.erase(lp+1);
    if(outSig!=NULL)
        snprintf(outSig,256,"com.x-plane.xtlua.%s.%s",plugin_base_path.c_str(),XTVERSION);

    // Construct xtlua_main modules on the X-Plane thread.
    loadXPScripts();

    findXTScripts();
    if(!loadXLua2Scripts())
    {
        // A failed script can already have registered generated XPLM
        // resources before its error or false XPluginStart result. Those
        // resources are owned by the host plug-in ID, not independently by
        // the Lua module, so continuing with the remaining modules would make
        // XPLM dispatch into a closed state. Refuse this plug-in load and let
        // XPLM discard every registration belonging to the failed load.
        cleanupScripts();
        xlua_callback_shutdown();
        g_xlua2_event_param_types.clear();
        while(xtlua_flush_log_queue() != 0) {}
        return 0;
    }
    threads.push_back(std::thread(do_during_physics));
    xtlua_flush_log_queue();

    return 1;
}

void	XTLuaXPluginStop(void)
{
    const HostTransitionScope transition;
    {
        std::lock_guard<std::mutex> lock(g_worker_state_mutex);
        run=false;
        active=false;
        g_worker_pause_requested=true;
    }
    g_worker_state_changed.notify_all();
    for (auto& th : threads)
        if(th.joinable())
            th.join();
    threads.clear();
    cleanupScripts();
    xlua_callback_shutdown();
    g_xlua2_event_param_types.clear();
    // X-Plane normally calls Disable first; Stop must not duplicate its line.
    if(g_plugin_enabled)
    {
        g_plugin_enabled=false;
        xtlua_queue_log("XTLua: host unlinked\n");
    }
    while(xtlua_flush_log_queue() != 0) {}
}

void XTLuaXPluginDisable(void)
{
    if(g_host_transition_active) return;
    const HostTransitionScope transition;
    const bool was_enabled = g_plugin_enabled;
    pauseWorker();
    g_plugin_enabled=false;
    for(module * m : xlua2_main_modules)
        m->xplugin_disable();
    if(was_enabled)
        xtlua_queue_log("XTLua: host unlinked\n");
    xtlua_flush_log_queue();
}

int XTLuaXPluginEnable(void)
{
    if(g_host_transition_active) return 0;
    if(g_plugin_enabled) return 1;
    const HostTransitionScope transition;
    xtlua_queue_log("XTLua: relinking DataRefs on X-Plane thread\n");
    xlua_relink_all_drefs();
    size_t enabled_count = 0;
    for(module * m : xlua2_main_modules)
    {
        if(!m->xplugin_enable())
        {
            // The DLL is one XPLM plug-in even though it hosts several Lua
            // modules.  It must not report itself enabled when any module has
            // rejected the transition: XPLM could otherwise call resources
            // registered by a Lua state which is deliberately disabled.
            while(enabled_count > 0)
                xlua2_main_modules[--enabled_count]->xplugin_disable();
            g_plugin_enabled=false;
            active=false;
            log_message(nullptr, "xlua2_main: %s: XPluginEnable failed or declined; host remains disabled\n",
                m->get_script_path().c_str());
            xtlua_flush_log_queue();
            return 0;
        }
        ++enabled_count;
    }
    g_plugin_enabled=true;
    resumeWorker(true);
    xtlua_queue_log("XTLua: host enabled; xtlua_worker active\n");
    xtlua_flush_log_queue();
    return 1;
}

void XTLuaXPluginReceiveMessage(
                    XPLMPluginID	inFromWho,
                    int				inMessage,
                    void *			inParam)
{
    if(inFromWho == XPLM_PLUGIN_XPLANE)
    {
    switch(inMessage) {
    case XPLM_MSG_LIVERY_LOADED:
        if(inParam != NULL)
            break;
        xlua_add_callout("livery_load");
        for(module * m : xlua2_main_modules)
            m->do_callout("livery_load");
        break;
    case XPLM_MSG_PLANE_LOADED:
        if(inParam != NULL)
            break;
        g_is_acf_inited = 0;
        xlua_relink_all_drefs();
        xlua_validate_drefs();
        xlua_setLoadStatus(1);
        xlua_add_callout("aircraft_load");
        break;
    case XPLM_MSG_PLANE_UNLOADED:
        if(inParam != NULL)
            break;
        if(g_is_acf_inited){
            for(vector<module *>::iterator m = xtlua_main_modules.begin(); m != xtlua_main_modules.end(); ++m)
                (*m)->acf_unload();
            for(module * m : xlua2_main_modules)
                m->acf_unload();

        }
        xlua_setLoadStatus(0);
        g_is_acf_inited = 0;
        break;
    case XPLM_MSG_AIRPORT_LOADED:
        if(g_xlua2_reload_on_flight_change && g_is_acf_inited)
        {
            g_xlua2_reload_requested=true;
            break;
        }
        if(!g_is_acf_inited)
        {
            // Pick up any last stragglers from out-of-order load and then validate our datarefs!
            xlua_relink_all_drefs();
            xlua_validate_drefs();
            xlua_setLoadStatus(1);
            xlua_add_callout("aircraft_load");
            for(vector<module *>::iterator m = xtlua_main_modules.begin(); m != xtlua_main_modules.end(); ++m)
                (*m)->acf_load();
            for(module * m : xlua2_main_modules)
                m->acf_load();
            g_is_acf_inited = 1;
        }

        xlua_add_callout("flight_start");
        for(vector<module *>::iterator m = xtlua_main_modules.begin(); m != xtlua_main_modules.end(); ++m)
            (*m)->flight_init();
        for(module * m : xlua2_main_modules)
            m->flight_init();
        break;
    case XPLM_MSG_PLANE_CRASHED:
        xlua_add_callout("flight_crash");
        for(vector<module *>::iterator m = xtlua_main_modules.begin(); m != xtlua_main_modules.end(); ++m)
            (*m)->flight_crash();
        for(module * m : xlua2_main_modules)
            m->flight_crash();
        break;
    }
    }
    // xlua2_main receives the full XPluginReceiveMessage stream on the
    // X-Plane thread; xtlua_worker/xtlua_main handling above is X-Plane-only.
    for(module * m : xlua2_main_modules)
        m->xplugin_receive_message(inFromWho, inMessage, inParam);
}
