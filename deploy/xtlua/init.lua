-- Wanna use STP with XLua?  Copy StackTracePlus.lua to be next to init.lua in the same folder
-- https://github.com/ignacio/StackTracePlus

-- Grab STP conditionally, do not squawk if it is missing.
-- if pcall(
	-- function()
		-- local STP_chunk = XLuaGetCode("../../StackTracePlus.lua")
		-- local STP = STP_chunk()
		-- debug.traceback = STP.stacktrace
	-- end)
-- then
	-- print("Using STP as debugger.")
-- end



function dump(value)
	local seen = {}
	local parts = {}
	local function append(item)
		if type(item) ~= "table" then
			parts[#parts + 1] = tostring(item)
		elseif seen[item] then
			parts[#parts + 1] = "<cycle>"
		else
			seen[item] = true
			parts[#parts + 1] = "{ "
			for key, child in pairs(item) do
				parts[#parts + 1] = "[" .. tostring(key) .. "] = "
				append(child)
				parts[#parts + 1] = ","
			end
			parts[#parts + 1] = "} "
			seen[item] = nil
		end
	end
	append(value)
	return table.concat(parts)
end

function isnan(value)
	return type(value) == "number" and value ~= value
end

--------------------------------------------------------------------------------
-- DATAREF LUA GLUE
--------------------------------------------------------------------------------
-- This code creates property objects (or MT-enhanced tables) for arrays that
-- let authors work with 


local function bulk_values(values)
	-- Classic module globals wrap plain tables into namespace proxies.
	local mt = type(values) == "table" and getmetatable(values)
	if mt and mt.__index == namespace_read then
		local stored = rawget(values, "values")
		local properties = rawget(values, "functions")
		if next(properties) == nil then return stored end
		local result = {}
		for key, value in pairs(stored) do result[key] = value end
		for key, property in pairs(properties) do result[key] = property.__get(property) end
		return result
	end
	return values
end

local array_methods = {}
function array_methods.get_values(self, offset, count)
	return XTLuaGetArrayValues(self.dref, offset, count)
end
function array_methods.set_values(self, values, offset)
	return XTLuaSetArrayValues(self.dref, bulk_values(values), offset)
end

function dref_array_read(array, key)
	if key == "len" then
		return XTLuaGetArrayLength(array.dref)
	end
	local method = array_methods[key]
	if method ~= nil then return method end
	local index = tonumber(key)
	if index == nil then return nil end
	return XTLuaGetArray(array.dref, index)
end

function dref_array_write(array, key, value)
	local index = tonumber(key)
	if index ~= nil then XTLuaSetArray(array.dref, index, value) end
end

local array_metatable = { __index = dref_array_read, __newindex = dref_array_write }
function wrap_dref_array(in_dref, dim)
	-- Length is queried dynamically; dim is retained only for call compatibility.
	return setmetatable({ dref = in_dref }, array_metatable)
end

function wrap_dref_number(in_dref)
	return {
		__get = function(self)
			return XTLuaGetNumber(self.dref)
		end,
		__set = function(self,v)
			XTLuaSetNumber(self.dref,v)
		end,
		dref = in_dref
	}
end

function wrap_dref_string(in_dref)
	return {
		__get = function(self)
			return XTLuaGetString(self.dref)
		end,
		__set = function(self,v)
			XTLuaSetString(self.dref,v)
		end,
		dref = in_dref
	}
end



function wrap_dref_any_deferred(in_dref)
	--[[
		This function builds a property object for a dataref where we do not
		know the type of dataref at wrapping time.  The callbacks inspect the 
		dataref on the fly and call the right callbacks basde on what they find.
		
		One special case: if we have an array we need to return a table so the client
		can then run the [] operator on the table.  We cache our table object after
		the first time we find it in "arr" and if askesd again we can just return it
		immediately and not leak out a huge number of tables to be GCed later.
		
		Right now I only expose the table in the read function, because in theory
		dr[4] = 5
		shows up as a "read" of DR in the global namespace - the write happens when the [4] = 5
		is applied to the subtable.  If we wake up one day and blow up in the write CB, 
		maybe I have reasoned wrong, but in the test code we write into the anonymous 
		dataref and it works.
	--]]
	return {
		__get = function(self)
			if self.arr ~= nil then
				return self.arr
			end
			local t = XTLuaGetDataRefType(self.dref)
			local b = string.find(t,"%[")
			if b ~= nil then
				local dim = tonumber(string.sub(t,b+1,-2))
				if dim == nil then
					return nil
				end
				self.arr = wrap_dref_array(self.dref,dim)
				return self.arr
			end
			if t == "string" then
				return XTLuaGetString(self.dref)
			elseif t == "number" then
				return XTLuaGetNumber(self.dref)
			else
				return 0.0
			end			
		end,
		__set = function(self,v)
			if self.arr ~= nil then
				return XTLuaSetArrayValues(self.dref, bulk_values(v))
			end
			local t = XTLuaGetDataRefType(self.dref)
			if string.find(t, "%[") ~= nil then
				self.arr = wrap_dref_array(self.dref)
				return XTLuaSetArrayValues(self.dref, bulk_values(v))
			end
			if t == "string" then
				return XTLuaSetString(self.dref,v)
			elseif t == "number" then
				return XTLuaSetNumber(self.dref,v)
			else
				return 0.0
				--error("Previously unresolved dataref is being written to but is an array or is still undefined.")
			end			
		end,	
		dref = in_dref,
		arr = nil,
	}	
end
	
function wrap_dref_any(dref,t)	
	local b = string.find(t,"%[")
	if b ~= nil then
		local dim = tonumber(string.sub(t,b+1,-2))
		if dim == nil then
			return nil
		end
		return wrap_dref_array(dref,dim)
	end
	if t == "string" then
		return wrap_dref_string(dref)
	end
	if t == "number" then
		return wrap_dref_number(dref)
	end
	return wrap_dref_any_deferred(dref)
end

function find_dataref(name)	
	local dref = XTLuaFindDataRef(name)
	local t = XTLuaGetDataRefType(dref)
	return wrap_dref_any(dref,t)
end

function create_dataref(name,type,notifier)
  error("create_dataref unsupported - use init script")
	--[[if notifier == nil then
		dref = XLuaCreateDataRef(name,type,"no",nil)
	else
		dref = XLuaCreateDataRef(name,type,"yes",notifier)
	end
	return wrap_dref_any(dref,type)
  ]]
end

--------------------------------------------------------------------------------
-- COMMAND LUA GLUE
--------------------------------------------------------------------------------

local command_methods = {}
local function command_handle(self)
	if self.cmd == nil then self.cmd = XTLuaFindCommand(self.name) end
	if self.cmd == nil then error("Unable to find command: " .. tostring(self.name), 3) end
	return self.cmd
end
function command_methods.start(self) XTLuaCommandStart(command_handle(self)) end
function command_methods.stop(self) XTLuaCommandStop(command_handle(self)) end
function command_methods.once(self) XTLuaCommandOnce(command_handle(self)) end
local command_metatable = { __index = command_methods }
function make_command_obj(in_cmd, in_name)
	return setmetatable({ cmd = in_cmd, name = in_name }, command_metatable)
end

function find_command(name)
	local c = XTLuaFindCommand(name)
	return make_command_obj(c,name)
end

function replace_command(name, func)
	local c = XTLuaFindCommand(name)
	XTLuaReplaceCommand(c,func)
	return make_command_obj(c,name)
end	

function wrap_command(name, before, after)
	local c = XTLuaFindCommand(name)
	XTLuaWrapCommand(c,before,after)
	return make_command_obj(c,name)
end

--------------------------------------------------------------------------------
-- TIMER UTILITIES
--------------------------------------------------------------------------------

function run_timer(func,delay,rep)
	local tobj = all_timers[func]
	if tobj == nil then
		tobj = XTLuaCreateTimer(func)
		all_timers[func] = tobj
	end
	XTLuaRunTimer(tobj,delay,rep)
end

function stop_timer(func)
	local tobj = all_timers[func]
	if tobj ~= nil then
		XTLuaRunTimer(tobj, -1.0, -1.0)
	end
end

function is_timer_scheduled(func)
	local tobj = all_timers[func]
	if tobj == nil then
		return false
	end
	return XTLuaIsTimerScheduled(tobj)
end

function get_timer_remaining(func)
	local tobj = all_timers[func]
	return tobj ~= nil and XTLuaGetTimerRemaining(tobj) or 0
end

function run_at_interval(func, interval)
	run_timer(func,interval,interval)
end

function run_after_time(func,delay)
	run_timer(func,delay,-1.0)
end

--------------------------------------------------------------------------------
-- NAMESPACE UTILITIES
--------------------------------------------------------------------------------
-- These put all script actions in a private namespace with meta-table 
-- enhancements.

function seems_like_prop(p)
	if type(p) ~= "table" then
		return false
	end
	local gfunc = rawget(p,"__get")
	local sfunc = rawget(p,"__set")
	if type(gfunc) ~= "function" then
		return false
	end
	if type(sfunc) ~= "function" then
		return false
	end
	return true
end

function seems_like_object(p)
	if type(p) ~= "table" then
		return false
	end
	if getmetatable(p) ~= nil	 then
		return true
	end
	for k,vv in pairs(p) do
		if type(vv) == "function" then
			return true
		end
	end
end


function namespace_ipairs(table, i)
	local function namespace_iter(table, i)
		i = i + 1
		local ftable = rawget(table,'functions')
		local vtable = rawget(table,'values')
		local fv = ftable[i]
		if fv ~= nil then
			return i,fv.__get(fv)
		else
			local vv = vtable[i]
			if vv ~= nil then
				return i,vv
			end
		end
	end
	return namespace_iter, table, 0
end

function namespace_len(table)
	local count = 0
	for _ in namespace_ipairs(table) do count = count + 1 end
	return count
end
  

function namespace_pairs(table, key, value)
	local function namespace_next(table, index)
		local ftable = rawget(table,'functions')
		local vtable = rawget(table,'values')
		if index == nil then
			local idx_f,key_f = next(ftable, nil)
			if idx_f == nil then
				local idx_v,key_v = next(vtable, nil)
				return idx_v,key_v
			else
				return idx_f,key_f.__get(key_f)
			end
		else
			if ftable[index] ~= nil then
				local idx_f,key_f = next(ftable,index)
				if idx_f == nil then
					return next(vtable, nil)
				else
					return idx_f,key_f.__get(key_f)
				end
			else
				return next(vtable, index)
			end
		end
	end

	return namespace_next, table, nil
end

function namespace_write(table, key, value)
	--print("Namespace write of "..key)
	local ftable = rawget(table,'functions')
	local vtable = rawget(table,'values')
	local rkeys = rawget(table,'raw_table_keys')

	local existing = vtable[key]
	if type(existing) == "table" and getmetatable(existing) == array_metatable and type(value) == "table" then
		XTLuaSetArrayValues(existing.dref, bulk_values(value))
		return
	end
	local func = ftable[key]
	if func ~= nil then
		func.__set(func,value)
	else
		if seems_like_prop(value) then
			ftable[key] = value
		else
			if not seems_like_object(value) and type(value) == "table" and getmetatable(value) == nil and rkeys[key] == nil then
				--print("Bare table wrap for "..key)
				local v = {
					functions = {},
					values = {},
					raw_table_keys = {},
					parent = nil
				}
				for k,vv in pairs(value) do
					if seems_like_prop(vv) then
						v.functions[k] = vv						
					else
						v.values[k] = vv
					end
				end
				setmetatable(v,getmetatable(table))
				vtable[key] = v
			else
				--print("copy for "..key)			
				vtable[key] = value
			end
		end
	end
end

function namespace_read(table,key)
	local ftable = rawget(table,'functions')
	local vtable = rawget(table,'values')
	local func = ftable[key]
	if func ~= nil then
		return func.__get(func)
	end
	local var = vtable[key]
	if var ~= nil then
		return var
	end
	local parent = rawget(table, 'parent')
	if parent ~= nil then
		return parent[key]
	end
	return nil
end

function create_namespace()
	local ret = {
		functions = {}, 
		values = {},
		raw_table_keys = {},
		create_prop = function(self,name, func)
			self.functions[name] = func
		end,
		parent = _G
	}
	-- TODO: use __len operator to restore # for Jim
	-- TODO: look at __pairs, __ipairs support
	local mt = { __index = namespace_read, __newindex = namespace_write, __pairs = namespace_pairs, __ipairs = namespace_ipairs, __len = namespace_len }
	setmetatable(ret,mt)
	return ret
end

-- Build a custom-built closure replacement for dofile that uses get-path to
-- find same-dir scripts and does a setfenv to our NS
function get_run_file_in_namespace(ns)
	return function(fname)
		local chunk = XLuaGetCode(fname)
		if type(chunk) ~= "function" then
			error("Unable to load '" .. tostring(fname) .. "': " .. tostring(chunk), 2)
		end
		setfenv(chunk, ns)
		return chunk()
	end
end

--------------------------------------------------------------------------------

-- SDK registration and synchronous filtering require an init/main module.
function create_command(name, desc, handler)
	error("create_command requires an xtlua_main init script", 2)
end
function filter_command(name, filter)
	error("filter_command is synchronous and requires an xtlua_main init script", 2)
end

function get_real_table_in_namespace(ns)
	return function(key, real_table)
		rawget(ns, 'values')[key] = real_table
	end
end
function get_raw_table_in_namespace(ns)
	return function(key)
		rawget(ns, 'raw_table_keys')[key] = true
	end
end

function run_module_in_namespace(fn)
	n = create_namespace()
	all_timers = { }
	
	n.make_prop = function(name,func)
		n:create_prop(name,func)
	end
	
	n.find_dataref = find_dataref
	n.create_command = create_command
	n.find_command = find_command
	n.wrap_command = wrap_command
	n.filter_command = filter_command
	n.replace_command = replace_command
	
	n.run_timer = run_timer
	n.stop_timer = stop_timer
	n.is_timer_scheduled = is_timer_scheduled
	n.get_timer_remaining = get_timer_remaining
	n.run_after_time = run_after_time
	n.run_at_interval = run_at_interval
	n.dofile = get_run_file_in_namespace(n)
	n.real_table = get_real_table_in_namespace(n)
	n.raw_table = get_raw_table_in_namespace(n)
	
	setfenv(fn,n)
	fn()
end

function setup_callback_var(var_name,var_value)
	n[var_name] = var_value
end

function do_callout(fname)
	local func=n[fname]
	if func ~= nil then
		if STP ~= nil then
			STP.add_known_function(func, fname)
		end
		func()
	end
end
