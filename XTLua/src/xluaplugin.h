//not for use inside a module
#ifndef xluaplugin_h
#define xluaplugin_h

#include <XPLMPlugin.h>
#include <XPLMUtilities.h>
#define PLUGINVERSION "2.4.9"
int     XTLuaXPluginStart(char *		outSig);
void	XTLuaXPluginStop(void);
void XTLuaXPluginDisable(void);
int XTLuaXPluginEnable(void);
void XTLuaXPluginReceiveMessage(
					XPLMPluginID	inFromWho,
					int				inMessage,
					void *			inParam);
typedef void * XPLMCommandRef;
typedef int XPLMCommandPhase;
int reloadScripts(XPLMCommandRef c, XPLMCommandPhase phase, void * ref);

#endif /* xluaplugin_h */
