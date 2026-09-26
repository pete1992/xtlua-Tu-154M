//not for use inside a module

#include <stdio.h>
#include <string.h>
#include <cstdlib>
#include "xluaplugin.h"
#include "shared_xpfuncs.h"
#include "XPLMMenus.h"
#include <string>

XPLMMenuID				PluginMenu = 0;
XPLMCommandRef			reload_cmd = nullptr;

bool isDebugInstance() { return false; }

#ifdef __linux__
#include <X11/Xlib.h>
static bool checkdisplayServer(){
	Display *display;
	int opcode, event,error;
	display = XOpenDisplay(NULL);
	if(!display)
		return true;//probably headless
	bool retVal=true;
	if(XQueryExtension(display,"XWAYLAND",&opcode, &event,&error)){
		log_message(nullptr, "XWayland is unsupported; a full X server is required\n");
		xtlua_flush_log_queue();
		retVal=false;
	}
	XCloseDisplay(display);
	return retVal;
}
#endif
int id=1;
enum eMenuItems : int
{
	MI_ResetState,
};
static void MenuHandler(void* menuRef, void* itemRef)
{
	switch ((eMenuItems)(size_t)itemRef)
	{
		case MI_ResetState:
			reloadScripts(reload_cmd, xplm_CommandBegin, nullptr);
			break;
	}
}
PLUGIN_API int XPluginStart(
						char *		outName,
						char *		outSig,
						char *		outDesc){

    //strcpy(outName, "XTLua " PLUGINVERSION);
	//char path_to_me_c[2048];
	//XPLMGetPluginInfo(XPLMGetMyID(), NULL, path_to_me_c, NULL, NULL);
	snprintf(outName, 256, "XTLua %s id%d", PLUGINVERSION, XPLMGetMyID());
    //strcpy(outSig, "com.x-plane.xtlua." VERSION);
    snprintf(outDesc, 256, "%s", "A minimal scripting environment for aircraft authors with multithreading.");
	bool isDebugMode=isDebugInstance();	
#ifdef __linux__
	xtlua_log_set_main_thread();
	if(!checkdisplayServer()){
		return 0;
	}
#endif
    if(isDebugMode){
		reload_cmd = XPLMCreateCommand("xtlua/reload_all_scripts", "Reload scripts and state for ");
		if (reload_cmd != nullptr)
		{
			XPLMRegisterCommandHandler(reload_cmd, reloadScripts, 1,  (void *)0);
		}
		int item = XPLMAppendMenuItem(XPLMFindPluginsMenu(), outName, nullptr, 0);
		PluginMenu = XPLMCreateMenu(outName, XPLMFindPluginsMenu(), item, MenuHandler, nullptr);
		XPLMAppendMenuItem(PluginMenu, "Reload Scripts", (void*)MI_ResetState, 0);
	}
    return XTLuaXPluginStart(outSig);


}

PLUGIN_API void	XPluginStop(void)
{
    XTLuaXPluginStop();
}

PLUGIN_API void XPluginDisable(void)
{
    XTLuaXPluginDisable();
}

PLUGIN_API int XPluginEnable(void)
{
    return XTLuaXPluginEnable();
}

PLUGIN_API void XPluginReceiveMessage(
					XPLMPluginID	inFromWho,
					int				inMessage,
					void *			inParam)
{
    XTLuaXPluginReceiveMessage(inFromWho,inMessage,inParam);
}
