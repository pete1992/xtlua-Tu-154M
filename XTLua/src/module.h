//
//  module.h
//  xlua
//
//  Created by Ben Supnik on 3/19/16.
// xTLua
// Modified by Mark Parker on 04/19/2020
//
//	Copyright 2016, Laminar Research
//	This source code is licensed under the MIT open source license.
//	See LICENSE.txt for the full terms of the license.

#ifndef module_h
#define module_h

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include <string>

using std::string;

struct module_alloc_block;

enum class module_runtime {
	legacy_worker,
	legacy_main,
	xlua2_main
};

enum class module_script_kind {
	legacy,
	xlua2,
	unsupported_xlua
};

class module {
public:

						 module(
							const char *		in_module_path,
							const char *		in_init_script,
							const char *		in_module_script,
							module_runtime		in_runtime);
						~module();

	static module_script_kind classify_script(const char * script_path);
	static module *		module_from_interp(lua_State * interp);
	static int			debug_proc_from_interp(lua_State * interp);

			bool		is_valid() const { return m_interp != NULL; }
			bool		is_xlua2() const { return m_runtime == module_runtime::xlua2_main; }
			bool		is_enabled() const { return m_enabled; }
			const string& get_script_path() const { return m_script_path; }

			bool		xplugin_start();
			bool		xplugin_enable();
			void		xplugin_disable();
			void		xplugin_stop();
			void		xplugin_receive_message(
							int inFromWho,
							int inMessage,
							void * inParam);

			void *		module_alloc_tracked(size_t amount);
			
			// Pushes error string or chunk onto interp stack, returns error code or 0.  
			int			load_module_relative_path(const string& path);
	
			void		acf_load();
			void		acf_unload();
			void		flight_init();
			void		flight_crash();
			
			int		pre_physics();
			int		post_physics();
			void		post_replay();
			int		do_callout(const char * call_name);

private:

		

	lua_State *				m_interp;
	module_alloc_block *	m_memory;
	string					m_path;
	string					m_script_path;
	int						m_debug_proc;
	module_runtime			m_runtime;
	bool					m_started;
	bool					m_enabled;

			void		shutdown_lua();
			int			do_direct_callout(const char * call_name);

	module();
	module(const module& rhs);
	module& operator=(const module& rhs);

};


#endif /* module_h */
