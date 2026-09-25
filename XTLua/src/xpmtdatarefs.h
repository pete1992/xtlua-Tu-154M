//
//  xpmtdatarefs.h
//  xTLua
//
//  Created by Mark Parker on 04/19/2020
//
//	Copyright 2020
//	This source code is licensed under the MIT open source license.
//	See LICENSE.txt for the full terms of the license.
#ifndef xpmtdatarefs_h
#define xpmtdatarefs_h

#include <XPLMDataAccess.h>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <vector>
#include <deque>
#define XPLM200 1
#include "xpcommands.h"
#include "xpmtdatatypes.h"
#include <XPLMProcessing.h> //XPLMGetElapsedTime
#include <XPLMUtilities.h>
#include <string>
#include <mutex>
#include "xpdatarefs.h"
struct	xtlua_dref {
	std::atomic<bool>        m_resolved{false}; // Release-published immutable binding metadata.
	xtlua_dref *				m_next=nullptr;
	std::string				m_name;
	XPLMDataRef				m_dref=nullptr;
	int						m_index=-1;	// -1 if index is NOT bound.
	XPLMDataTypeID			m_types=0;
	int						m_ours=0;		// 1 if we made, 0 if system
    xlua_dref *				local_dref=nullptr;
	//xtlua_dref_notify_f		m_notify_func;
	//void *					m_notify_ref;
	
	// IF we made the dataref, this is where our storage is!
	//double					m_number_storage;
	//vector<double>			m_array_storage;
	std::string					m_string_storage;
};

struct xtlua_cmd {
	xtlua_cmd() : m_next(NULL),m_cmd(NULL),m_ours(0),
		m_pre_handler(NULL),m_pre_ref(NULL),
		m_main_handler(NULL),m_main_ref(NULL),
		m_post_handler(NULL),m_post_ref(NULL),m_down_time(0.0f) { }

	xtlua_cmd *			m_next;
	std::string				m_name;
	XPLMCommandRef		m_cmd;
	int					m_ours;
	xtlua_cmd_handler_f	m_pre_handler;
	void *				m_pre_ref;
	xtlua_cmd_handler_f	m_main_handler;
	void *				m_main_ref;
	xtlua_cmd_handler_f	m_post_handler;
	void *				m_post_ref;
	float				m_down_time;
};
struct xlua_cmd {
	xlua_cmd() : m_next(NULL),m_cmd(NULL),
    m_main_handler(NULL),m_main_ref(NULL),m_down_time(0.0f) { }

	xlua_cmd *			m_next;
	std::string				m_name;
	XPLMCommandRef		m_cmd;
    xlua_cmd_handler_f	m_main_handler;
	void *				m_main_ref;
    float				m_down_time;
    xlua_cmd_handler_f m_pre_handler=nullptr;
    void * m_pre_ref=nullptr;
    xlua_cmd_handler_f m_post_handler=nullptr;
    void * m_post_ref=nullptr;
    xlua_cmd_filter_f m_filter=nullptr;
    void * m_filter_ref=nullptr;
    unsigned m_hold_count=0;
    bool m_filter_allow=true;
    bool m_before_registered=false;
    bool m_after_registered=false;
    unsigned m_dispatch_depth=0;
    bool m_has_seen_begin=false;
    std::uint64_t m_next_hold=0;
    struct Hold {
        std::uint64_t serial=0;
        bool notify=false; // Recursive SDK Begins are recorded but never invoke Lua.
        bool allowed=false;
        bool admitted=false; // Published only after the Begin's before callbacks return normally.
        float down_time=0.0f;
        xlua_cmd_handler_f pre_handler=nullptr;
        void * pre_ref=nullptr;
        xlua_cmd_handler_f main_handler=nullptr;
        void * main_ref=nullptr;
        xlua_cmd_handler_f post_handler=nullptr;
        void * post_ref=nullptr;
    };
    std::vector<Hold> m_holds; // LIFO balance; SDK phases do not identify their event source.
    struct PostSnapshot {
        xlua_cmd_handler_f handler=nullptr;
        void * ref=nullptr;
        float down_time=0.0f;
        bool valid=false;
    };
    PostSnapshot m_post_snapshots[3]; // One admitted snapshot per SDK phase; no filter reread after dispatch.
};
/*static int xlua_std_pre_handler(XPLMCommandRef c, XPLMCommandPhase phase, void * ref);
static int xlua_std_main_handler(XPLMCommandRef c, XPLMCommandPhase phase, void * ref);
static int xlua_std_post_handler(XPLMCommandRef c, XPLMCommandPhase phase, void * ref);*/
class NavAid
{
    public:
    int    id;
    int    type;
    float  latitude;    /* Can be NULL */
    float  longitude;  
    int    frequency;
    float  heading;
    std::string name;
    std::string ident;
    NavAid * next;
};
class XTLuaDataRefs
{
private:
    struct ArrayBridgeRef {
        XPLMDataRef ref=nullptr;
        int type=0;
        bool active=false;
    };
    // Handles are already stable cache identities; never format/hash pointer text
    // on the worker's per-element/scalar access path.
    std::unordered_map<XPLMDataRef, std::vector<XTLuaArrayFloat*> > floatdataRefs;
    std::unordered_map<XPLMDataRef, ArrayBridgeRef> arrayDataRefs;
    std::unordered_map<XPLMDataRef, std::unordered_map<int, double> > deferredArrayWrites;
    void updateArrayDataRef(XPLMDataRef key); // Main thread; SDK calls outside data_mutex.
    void updateStringDataRefsImpl();
    void updateFloatDataRefsImpl();
    void updateNavDataRefsImpl();
    void updateCommandsImpl();
    //std::unordered_map<std::string, XTLuaDouble> doubledataRefs;
    //std::unordered_map<std::string, XTLuaInteger> intdataRefs;
    //std::unordered_map<std::string, XTLuaChars> stringdataRefs;
    std::unordered_map<XPLMDataRef, XTLuaCharArray*> stringdataRefs;
    std::vector<xtlua_dref*> drefResolveQueue;
    std::vector<xtlua_cmd*> cmdResolveQueue;
    std::unordered_set<xtlua_cmd*> cmdHandlerResolveQueue;
    std::unordered_map<int, NavAid*> localNavaids;
    std::string incomingNavaidString;
    std::string localNavaidString;
    std::string incomingFMSString;
    std::string localFMSString;
    std::string currentDisplayedEntry;
    //std::unordered_map<std::string, XTCmd> startCmds;
    //std::unordered_map<std::string, XTCmd> stopCmds;
    //std::unordered_map<std::string, XTCmd> fireCmds;
    enum class CommandAction { begin, end, once };
    struct CommandRequest {
        std::string name;
        CommandAction action;
    };
    struct MainThreadRequest {
        std::string name;
        std::string value;
    };
    std::deque<CommandRequest> commandQueue;
    std::vector<MainThreadRequest> mainThreadQueue;
    std::unordered_map<std::string, XPLMCommandRef> resolvedCommands; // Main thread only; SDK refs live for plugin lifetime.
    std::unordered_map<XPLMCommandRef, unsigned> heldCommands; // Main thread only.
    std::unordered_set<std::string> unresolvedCommands; // Main-thread diagnostic suppression.
    bool acceptingRequests=true; // Protected by data_mutex.
    bool cleaning=false;
    void updateMainThreadRequests();
    void applyMainThreadRequest(const MainThreadRequest& request);
    // Main-thread-only dispatch state. Reentrant SDK callbacks must defer a
    // second drain and lifecycle cleanup until the current batch has returned.
    unsigned mainDispatchDepth=0;
    bool updatingDataRefs=false;
    bool refreshingDataRefs=false;
    bool updatingStrings=false;
    bool updatingFloats=false;
    bool updatingNav=false;
    bool updatingCommands=false;
    bool updatingMainRequests=false;
    bool resolving=false;
    std::vector<XTControlObject*> controlOverrides;
    std::atomic<double> timeT{0.0};
    NavAid * navaids=NULL;
    NavAid * lastnavaid=NULL;
    XPLMDataRef latR=nullptr;
    XPLMDataRef lonR=nullptr;
    double lat=0.0;
    double lon=0.0;
    double lastUpdatelat=0.0;
    double lastUpdatelon=0.0;
    NavAid * current_navaid=NULL;
    bool skipNaviads=true;
    int updateRoll=0;
public:
    bool isMainDispatchActive() const { return mainDispatchDepth != 0; }
    std::atomic<int> isPaused{1};
    double simTime=0;
    double beginFlightTime=0;
    std::atomic<int> isLoaded{0};
    XPLMDataRef paused_ref=NULL;
    XPLMDataRef replay_ref=NULL;
    XPLMDataRef sim_time_ref=NULL;
    void XTCommandBegin(xtlua_cmd * cmd);
    void XTCommandEnd(xtlua_cmd * cmd);
    void XTCommandOnce(xtlua_cmd * cmd);
    void XTRegisterCommandHandler(xtlua_cmd * cmd);
    double XTGetElapsedTime();
    //void ShowDataRefs();
    void updateDataRefs();
    void refreshAllDataRefs();
    void updateStringDataRefs();
    void updateFloatDataRefs();
    void updateNavDataRefs();
    void update_localNavData();
    void updateCommands();
    void cleanup();
    void                 XTqueueresolve_dref(xtlua_dref * d);//can be called from anywhere
    void                 XTqueueresolve_cmd(xtlua_cmd * d);//can be called from anywhere
    int                 resolveQueue();//only to be called from flight loop thread
    std::vector<xtlua_cmd*> XTGetHandlers();
    int                  XTGetDatavf(
                                   xtlua_dref * d,    
                                   double *             outValues,    /* Can be NULL */
                                   int                  inOffset,    
                                   int                  inMax,bool local);
    void                 XTSetDatavf(
                                   xtlua_dref * d,    
                                   double               inValue,
                                   int                  index);                               
    // Worker-side array snapshots and writes: one bridge lock per range.
    std::vector<double>  XTGetArrayValues(xtlua_dref * d, int offset, int count);
    int                  XTSetArrayValues(xtlua_dref * d, const std::vector<double>& values, int offset);
                                  
    double               XTGetDataf(
                                   xtlua_dref * d,bool local);
    void                 XTSetDataf(
                                   xtlua_dref * d,    
                                   double               inValue,bool local);
    
    
    int                  XTGetDatab(
                                   xtlua_dref * d,    
                                   void *               outValue,    /* Can be NULL */
                                   int                  inOffset,    
                                   int                  inMaxBytes,bool local);
    std::string          XTGetString(xtlua_dref * d);
    void                 XTSetDatab(
                                   xtlua_dref * d,    
                                   std::string value);                                                             
};

#endif /* xpmtdatarefs_h */
