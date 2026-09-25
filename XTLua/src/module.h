//
//  module.h
//  xlua / xTLua module runtime
//
//  Created by Ben Supnik on 3/19/16.
//
//  XTLua
//  Modified by Mark Parker on 04/19/2020
//  Runtime naming/comments updated for xtlua_worker/xtlua_main/xlua2_main
//  by Peter Schwake on 21/09/2026
//
//	Copyright 2016, Laminar Research
//	This source code is licensed under the MIT open source license.
//	See LICENSE.txt for the full terms of the license.
//
//------------------------------------------------------------------------------
//  RUNTIME MODEL
//
//  xtlua_worker
//      Core XTLua runtime. Lua system logic, including before_physics() and
//      after_physics(), runs asynchronously on the XTLua worker thread.
//      Direct XPLM access must not be exposed through this runtime; simulator
//      state and actions are exchanged through the buffered XTLua interfaces.
//
//  xtlua_main
//      Main-thread companion runtime used by XTLua for work that must remain on
//      the X-Plane thread. It uses the direct XLua-compatible binding set and
//      keeps the traditional private module namespace.
//
//  xlua2_main
//      XLua 2 / SDK 4.4 runtime. Executes on the X-Plane main thread and may use
//      the generated direct XPLM bindings. XLua 2 modules execute in the Lua
//      state's global namespace, matching the XLua 2 contract.
//------------------------------------------------------------------------------

#ifndef module_h
#define module_h

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include <cstddef>
#include <string>

using std::string;

struct module_alloc_block;

// Execution environment of one loaded Lua module.
enum class module_runtime {
	xtlua_worker,  // Asynchronous XTLua worker-thread runtime.
	xtlua_main,    // XTLua main-thread companion / direct XLua-compatible API.
	xlua2_main     // XLua 2 / SDK 4.4 main-thread runtime with direct XPLM glue.
};

// Script format detected from the module's first-line marker.
enum class module_script_kind {
	xtlua,            // Standard XTLua module; no XLua 2 marker required.
	xlua2,            // Exact supported marker: --[[ XLua 2.0 ]]
	unsupported_xlua  // Recognized XLua marker that this host does not support.
};

class module {
public:
	module(
		const char * in_module_path,
		const char * in_init_script,
		const char * in_module_script,
		module_runtime in_runtime);
	~module();

	// Classifies a module before construction so the host can select the
	// appropriate runtime without executing aircraft code first.
	static module_script_kind classify_script(const char * script_path);

	// Recovers the owning module from the private Lua-registry entry installed
	// during construction. The registry entry is authoritative; aircraft code
	// cannot replace it by overwriting a normal Lua global.
	static module * module_from_interp(lua_State * interp);
	static int debug_proc_from_interp(lua_State * interp);

	bool is_valid() const { return m_interp != NULL; }
	bool is_xlua2() const { return m_runtime == module_runtime::xlua2_main; }
	module_runtime get_runtime() const { return m_runtime; }
	bool is_enabled() const { return m_enabled; }
	const string& get_script_path() const { return m_script_path; }

	// XLua 2 plug-in lifecycle. These are meaningful only for xlua2_main;
	// other runtimes are driven by the normal XTLua callout path below.
	bool xplugin_start();
	bool xplugin_enable();
	void xplugin_disable();
	void xplugin_stop();
	void xplugin_receive_message(
		int inFromWho,
		int inMessage,
		void * inParam);

	// Lifetime-bound storage for callback records used by the classic XTLua/
	// XLua-compatible bindings. Memory is reclaimed with the module.
	void * module_alloc_tracked(size_t amount);

	// Loads a file relative to the module directory onto the Lua stack.
	// Returns a Lua load error code or 0 on success.
	int load_module_relative_path(const string& path);

	// Aircraft/runtime callouts. For xtlua_worker these are executed by the
	// asynchronous worker; xtlua_main and xlua2_main execute on the main thread.
	void acf_load();
	void acf_unload();
	void flight_init();
	void flight_crash();
	int pre_physics();
	int post_physics();
	void post_replay();
	int do_callout(const char * call_name);

private:
	lua_State * m_interp;
	module_alloc_block * m_memory;
	string m_path;
	string m_script_path;
	int m_debug_proc;
	module_runtime m_runtime;
	bool m_started;
	bool m_enabled;

	void shutdown_lua();

	// Direct global-function dispatch used by xlua2_main. The private namespace
	// runtimes (xtlua_worker/xtlua_main) go through do_callout() instead.
	int do_direct_callout(const char * call_name);

	module();
	module(const module& rhs);
	module& operator=(const module& rhs);
};

#endif /* module_h */
