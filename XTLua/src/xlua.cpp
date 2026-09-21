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
#define XTVERSION "2.4.7"
#include <thread>
#ifndef XPLM200
#define XPLM200
#endif
#ifndef XPLM210
#define XPLM210
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
using std::vector;

/*
    TODO: get good errors on compile error.
    TODO: pipe output somewhere useful.
 */
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#if !MOBILE
//static void *			g_alloc = NULL;
#endif
static vector<module *>g_modules; //modules in a thread
static vector<module *>xp_modules; //xp safe modules
static vector<module *>xlua2_modules; //SDK 4.4 modules, always main-thread
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
static std::atomic_bool g_xlua2_reload_requested{false};
static bool g_xlua2_reload_on_flight_change = false;
static bool g_plugin_enabled = false;
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
    if(g_xlua2_reload_requested.exchange(false))
    {
        XPLMReloadThisPlugin(false);
        return 0.0f;
    }

    xlua_do_timers_for_time(xlua_get_simulated_time(),xlua_ispaused());

if(loadedModules&&xtlua_dref_resolveDREFQueue()==0&&!ready){
        ready=true;
        printf("x(t)lua set ready\n");
    }
    if(ready)
        xtlua_dref_preUpdate();

    if(XPLMGetDatai(g_replay_active) == 0 &&
       XPLMGetDataf(g_sim_period) > 0.0f)
    {
        for(module * m : xlua2_modules)
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
            for(vector<module *>::iterator m = xp_modules.begin(); m != xp_modules.end(); ++m)
                (*m)->post_physics();
            for(module * m : xlua2_modules)
                m->post_physics();
        }
    }
    else
    {
        for(module * m : xlua2_modules)
            m->post_replay();
    }
    if(ready)
        xtlua_dref_postUpdate();
    liveThread=true;
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
    //begin xtlua
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
            else if(script_kind == module_script_kind::legacy)
            {
                mod_paths.push_back(mod_path);
                script_paths.push_back(script_path);
            }
            else
            {
                string message = "XTLua: refusing unsupported XLua marker in ";
                message += script_path;
                message += "; use exact first line --[[ XLua 2.0 ]]\n";
                XPLMDebugString(message.c_str());
            }
        }
        ++offset;
        if(offset == mf)
            break;
    }

    dirtyXTScripts=true;
}
static void loadXTScripts(){
    printf("begin loading scripts %d\n",myID);
    printf("%s\n",init_script_path.c_str());
    for(int i=0;i<static_cast<int>(script_paths.size());i++)
    {
            printf(" loading %s\n",script_paths[i].c_str());
#if !MOBILE
            g_modules.push_back(new module(
                            mod_paths[i].c_str(),
                            init_script_path.c_str(),
                            script_paths[i].c_str(),
                            module_runtime::legacy_worker));
#else
            g_modules.push_back(new module(
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
            xlua2_modules.push_back(candidate);
        }
        else
        {
            XPLMDebugString("XTLua: XLua 2 module failed during load/start; refusing host startup\n");
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
            xp_modules.push_back(new module(
                            mod_path.c_str(),
                            init_script_path.c_str(),
                            script_path.c_str(),
                            module_runtime::legacy_main));
        }
        ++offset;
        if(offset == mf)
            break;
    }
}

static void cleanupScripts(){
    if(g_pre_loop!=NULL)
        XPLMDestroyFlightLoop(g_pre_loop);
    if(g_post_loop!=NULL)
        XPLMDestroyFlightLoop(g_post_loop);
    g_pre_loop = NULL;
    g_post_loop = NULL;
    if(g_is_acf_inited)
    {
        for(module * m : xp_modules)
            m->acf_unload();
        for(module * m : xlua2_modules)
            m->acf_unload();
    }
    g_is_acf_inited = 0;

    // XLua 2 shutdown hooks run while XPLM is still available. Each module
    // then unregisters its direct command handlers and timers before closing
    // its Lua state.
    for(module * m : xlua2_modules)
        delete m;
    xlua2_modules.clear();

    xtlua_dref_cleanup();
    xtlua_cmd_cleanup();
    xtlua_timer_cleanup();
    for(vector<module *>::iterator m = xp_modules.begin(); m != xp_modules.end(); ++m)
        delete (*m);
    xp_modules.clear();
    for(vector<module *>::iterator m = g_modules.begin(); m != g_modules.end(); ++m)
        delete (*m);
    g_modules.clear();
    script_paths.clear();
    mod_paths.clear();
    xlua2_script_paths.clear();
    xlua2_mod_paths.clear();
}

int reloadScripts(XPLMCommandRef c, XPLMCommandPhase phase, void * ref){
    if(phase ==0){
        // Generic SDK resources created by XLua 2 (windows, flight loops,
        // accessors, map hooks, ...) cannot all be reconstructed safely by
        // the legacy in-process script reload. Let SDK 4.4 reload the entire
        // plug-in so XPLM first removes every resource owned by this plug-in.
        if(!xlua2_modules.empty())
        {
            g_xlua2_reload_requested=true;
            return 0;
        }

        //pause XT thread
        printf("XTLua going to sleep for scripts reload\n");
        active=false;
        while(!sleeping)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        printf("XTLua sleeping for scripts reload\n");
        loadedModules=false;
        ready=false;
        cleanupScripts();
        loadXPScripts();
        xlua_relink_all_drefs();
        findXTScripts();
        if(!loadXLua2Scripts())
        {
            // This can only be reached when XLua 2 was added since the last
            // discovery. We are already in a main-thread command callback;
            // reload immediately because the legacy master loop was removed
            // by cleanupScripts() and can no longer service a deferred flag.
            XPLMReloadThisPlugin(false);
            return 0;
        }

        while(dirtyXTScripts)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if(g_plugin_enabled)
        {
            size_t enabled_count = 0;
            for(module * m : xlua2_modules)
            {
                if(!m->xplugin_enable())
                {
                    while(enabled_count > 0)
                        xlua2_modules[--enabled_count]->xplugin_disable();
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
        registerFlightLoop();
        active=true;
        printf("XLua active with new scripts\n");
    }

    return 0;
}
void do_during_physics(){
    while(myID==0&&run){
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if(!run){
        sleeping=true;
        return;
    }
    printf("during_physics thread open %d\n",myID);
    while(!liveThread&&run){
        std::this_thread::sleep_for(std::chrono::seconds(1));
        sleeping=true;
    }
    if(!run){
        sleeping=true;
        return;
    }
    sleeping=false;
    printf("during_physics thread woke up %d\n",myID);
    loadXTScripts();

    loadedModules=true;
    while(liveThread&&run&&!ready){
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    while(liveThread&&run){


        if(active&&!dirtyXTScripts){
            try{

                sleeping=false;
                auto start = std::chrono::steady_clock::now();
                std::vector<XTCmd*> runItems=get_runQueue();
                for(XTCmd* item:runItems){
                    item->runFunc(item->xluaref, item->phase, item->duration, item->m_func_ref);
                    delete item;
                }
                xtlua_do_timers_for_time(xlua_get_simulated_time(),xlua_ispaused());
                std::vector<string> msgItems=get_runMessages();

                for(string item:msgItems){
                        printf("XTLua:do threaded callout %s\n",item.c_str());

                        for(vector<module *>::iterator m = g_modules.begin(); m != g_modules.end(); ++m)
                            (*m)->do_callout(item.c_str());
                    }
                if(!xlua_ispaused()){
                    for(vector<module *>::iterator m = g_modules.begin(); m != g_modules.end(); ++m){
                        int ret=(*m)->pre_physics();
                        if(ret!=0)
                            active=false;
                    }

                    for(vector<module *>::iterator m = g_modules.begin(); m != g_modules.end(); ++m)
                    {
                        int ret=(*m)->post_physics();
                        if(ret!=0)
                            active=false;
                    }
                }

                xtlua_localNavData();
                auto finish = std::chrono::steady_clock::now();
                auto elapsed = finish - start;
                auto min_frame = std::chrono::milliseconds(20);
                if (elapsed < min_frame)
                    std::this_thread::sleep_for(min_frame - elapsed);//50fps or less
                else
                {
                    int diff = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
                    if(diff > 30) printf("warn: xtlua time overflow!=%d\n", diff);
                }

            }catch(...){
                printf("Exception\n");
                active=false;
            }
        }
        else{
            sleeping=true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if(dirtyXTScripts){
                printf("XTLua:do Load Scripts\n");
                loadXTScripts();
                loadedModules=true;
                printf("XTLua:Load Scripts complete, waiting for ready signal\n");
                while(liveThread&&run&&!ready){
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                printf("XTLua:exit sleep\n");
            }
        }
    }

    if(g_is_acf_inited)
    {
        for(vector<module *>::iterator m = g_modules.begin(); m != g_modules.end(); ++m)
            (*m)->acf_unload();
    }
    sleeping=true;
    printf("XTLua:new during_physics thread stopped %d\n",myID);
}
std::vector<std::thread> threads;
int XTLuaXPluginStart(char * outSig)
{
    ready=false;
    loadedModules=false;
    liveThread=false;
    run=true;
    active=false;
    dirtyXTScripts=false;
    sleeping=false;
    g_plugin_enabled=false;
    g_xlua2_reload_requested=false;
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

    //do create datarefs on thread
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
        return 0;
    }
    printf("XTLua XTLuaXPluginStart %s\n",outSig != NULL ? outSig : "");
    threads.push_back(std::thread(do_during_physics));

    return 1;
}

void	XTLuaXPluginStop(void)
{
    run=false;
    for (auto& th : threads)
        if(th.joinable())
            th.join();
    threads.clear();
    cleanupScripts();
    xlua_callback_shutdown();
    g_xlua2_event_param_types.clear();
    printf("XTLua XTLuaXPluginStop %d\n",myID);
}

void XTLuaXPluginDisable(void)
{
    printf("XTLua going to sleep in XTLuaXPluginDisable\n");
    XPLMDebugString("XTLua going to sleep in XTLuaXPluginDisable\n");
    active=false;
    while(!sleeping&&liveThread&&run){

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    for(module * m : xlua2_modules)
        m->xplugin_disable();
    g_plugin_enabled=false;
    printf("XTLua sleeping\n");
}

int XTLuaXPluginEnable(void)
{
    printf("XTLua active %d\n", XPLMGetMyID());
    XPLMDebugString("XTLua: XLua relink all Drefs\n");
    xlua_relink_all_drefs();
    size_t enabled_count = 0;
    for(module * m : xlua2_modules)
    {
        if(!m->xplugin_enable())
        {
            // The DLL is one XPLM plug-in even though it hosts several Lua
            // modules.  It must not report itself enabled when any module has
            // rejected the transition: XPLM could otherwise call resources
            // registered by a Lua state which is deliberately disabled.
            while(enabled_count > 0)
                xlua2_modules[--enabled_count]->xplugin_disable();
            g_plugin_enabled=false;
            active=false;
            XPLMDebugString("XTLua: an XLua 2 module declined XPluginEnable; host remains disabled\n");
            return 0;
        }
        ++enabled_count;
    }
    g_plugin_enabled=true;
    active=true;
    XPLMDebugString("XTLua: XTLua active\n");
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
        for(module * m : xlua2_modules)
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
            for(vector<module *>::iterator m = xp_modules.begin(); m != xp_modules.end(); ++m)
                (*m)->acf_unload();
            for(module * m : xlua2_modules)
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
            for(vector<module *>::iterator m = xp_modules.begin(); m != xp_modules.end(); ++m)
                (*m)->acf_load();
            for(module * m : xlua2_modules)
                m->acf_load();
            g_is_acf_inited = 1;
        }

        xlua_add_callout("flight_start");
        for(vector<module *>::iterator m = xp_modules.begin(); m != xp_modules.end(); ++m)
            (*m)->flight_init();
        for(module * m : xlua2_modules)
            m->flight_init();
        break;
    case XPLM_MSG_PLANE_CRASHED:
        xlua_add_callout("flight_crash");
        for(vector<module *>::iterator m = xp_modules.begin(); m != xp_modules.end(); ++m)
            (*m)->flight_crash();
        for(module * m : xlua2_modules)
            m->flight_crash();
        break;
    }
    }

    // XLua 2 mirrors the native plugin contract and therefore sees messages
    // from X-Plane as well as from other plugins. Legacy XTLua handling above
    // intentionally remains restricted to X-Plane messages.
    for(module * m : xlua2_modules)
        m->xplugin_receive_message(inFromWho, inMessage, inParam);
}
