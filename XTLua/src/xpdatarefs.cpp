//
//  xpdatarefs.cpp
//  xlua
//
//  Created by Ben Supnik on 3/20/16.
//
//	Copyright 2016, Laminar Research
//	This source code is licensed under the MIT open source license.
//	See LICENSE.txt for the full terms of the license.

// xTLua
// Modified by Mark Parker on 04/19/2020
#include <cstdio>
#include <cmath>
#include "xpcommands.h"
#include "xptimers.h"
#define XPLM200 1
#include <XPLMUtilities.h>
//#include <XPLMProcessing.h>
#include "xpdatarefs.h"

//#include <XPLMDataAccess.h>
#include <XPLMPlugin.h>

#include <vector>
#include <assert.h>
#include <algorithm>
#include <limits>
#include <mutex>
#include <atomic>
#include <memory>
#include <unordered_set>


#include <XPLMUtilities.h>
#include <XPLMProcessing.h>
#include "xpmtdatarefs.h"
using std::min;
using std::max;
using std::vector;

#define MSG_ADD_DATAREF 0x01000000
#if !MOBILE
#define STAT_PLUGIN_SIG "xplanesdk.examples.DataRefEditor"
#endif

//#define TRACE_DATAREFS printf
#define TRACE_DATAREFS(...)

static int xlua_round_to_int(double value)
{
	if(std::isnan(value))
		return 0;
	double rounded = std::round(value);
	if(rounded >= static_cast<double>((std::numeric_limits<int>::max)()))
		return (std::numeric_limits<int>::max)();
	if(rounded <= static_cast<double>((std::numeric_limits<int>::min)()))
		return (std::numeric_limits<int>::min)();
	return static_cast<int>(rounded);
}

static XTLuaDataRefs xtluaDefs;

static std::atomic<bool> active{false}; // main publishes SDK accessor availability
struct	xlua_dref {
	xlua_dref *				m_next;
	string					m_name;
	XPLMDataRef				m_dref;
	int						m_index;	// -1 if index is NOT bound.
	XPLMDataTypeID			m_types;
	int						m_ours;		// 1 if we made, 0 if system
	xlua_dref_notify_f		m_notify_func;
	void *					m_notify_ref;
	//bool 					changed;
	// IF we made the dataref, this is where our storage is!
	double					m_number_storage;
	vector<double>			m_array_storage;
	string					m_string_storage;
};
static std::unordered_set<xlua_dref*> changedDrefs;
static xlua_dref *		l_drefs = NULL;


static xtlua_dref *		s_drefs = NULL;
static std::mutex worker_dref_list_mutex;

static std::vector<xtlua_dref *> worker_drefs_snapshot()
{
	std::lock_guard<std::mutex> lock(worker_dref_list_mutex);
	std::vector<xtlua_dref *> result;
	for(xtlua_dref * d = s_drefs; d; d = d->m_next)
		result.push_back(d);
	return result;
}
static std::mutex xlua_data_mutex;
static std::mutex xlua_change_mutex;
//for change locking
static void	xlua_dref_changed(xlua_dref * r){
	std::lock_guard<std::mutex> lock(xlua_change_mutex);
	changedDrefs.insert(r);
}

// For nunmbers
static int	xlua_geti(void * ref)
{
	if(!active)
		return 0;
	
	xlua_dref * r = (xlua_dref *) ref;
	assert(r->m_ours);
	xlua_data_mutex.lock();	
	int retVal=xlua_round_to_int(r->m_number_storage);
	xlua_data_mutex.unlock();
	return retVal;
}

static void	xlua_seti(void * ref, int v)
{
	if(!active)
		return;
	xlua_dref * r = (xlua_dref *) ref;
	assert(r->m_ours);
	double vv = v;
	//r->changed=false;
	xlua_data_mutex.lock();	
	if(r->m_number_storage != vv)
	{
		r->m_number_storage = vv;
		xlua_dref_changed(r);
	}
	xlua_data_mutex.unlock();
	//if(changed && r->m_notify_func)
	//	r->m_notify_func(r,r->m_notify_ref);	
}

static float xlua_getf(void * ref)
{
	if(!active)
		return 0;
	xlua_dref * r = (xlua_dref *) ref;
	assert(r->m_ours);
	xlua_data_mutex.lock();	
	float retVal= static_cast<float>(r->m_number_storage);
	xlua_data_mutex.unlock();
	return retVal;
}

static void	xlua_setf(void * ref, float v)
{
	if(!active)
		return;
	xlua_dref * r = (xlua_dref *) ref;
	assert(r->m_ours);
	double vv = v;
	//r->changed=false;
	xlua_data_mutex.lock();	
	if(r->m_number_storage != vv)
	{
		r->m_number_storage = vv;
		xlua_dref_changed(r);
	}
	xlua_data_mutex.unlock();
	//if(changed && r->m_notify_func)
	//	r->m_notify_func(r,r->m_notify_ref);
}

static double xlua_getd(void * ref)
{
	if(!active)
		return 0;
	xlua_dref * r = (xlua_dref *) ref;
	assert(r->m_ours);
	xlua_data_mutex.lock();	
	double retVal=r->m_number_storage;
	xlua_data_mutex.unlock();
	return retVal;
}

static void	xlua_setd(void * ref, double v)
{
	if(!active)
		return;
	xlua_dref * r = (xlua_dref *) ref;
	assert(r->m_ours);
	double vv = v;
	//bool changed = false;
	xlua_data_mutex.lock();
	if(r->m_number_storage != vv)
	{
		r->m_number_storage = vv;
		xlua_dref_changed(r);
	}
	
	xlua_data_mutex.unlock();
	//if(changed && r->m_notify_func)
	//	r->m_notify_func(r,r->m_notify_ref);
}

// For arrays
static int xlua_getvi(void * ref, int * values, int offset, int max)
{
	if(!active)
		return 0;
	xlua_dref * r = (xlua_dref *) ref;
	xlua_data_mutex.lock();
	size_t count=0;
	assert(r->m_ours);
	if(values == NULL){
		count= r->m_array_storage.size();
		xlua_data_mutex.unlock();
		return (int)count;
	}
	if(offset < 0 || max <= 0 || static_cast<size_t>(offset) >= r->m_array_storage.size()){
		xlua_data_mutex.unlock();
		return 0;
	}
	count = (std::min)(static_cast<size_t>(max), r->m_array_storage.size() - static_cast<size_t>(offset));
	for(size_t i = 0; i < count; ++i)
		values[i] = xlua_round_to_int(r->m_array_storage[i + static_cast<size_t>(offset)]);
	xlua_data_mutex.unlock();
	return (int)count;
}

static void xlua_setvi(void * ref, int * values, int offset, int max)
{
	if(!active)
		return;
	if(values == NULL || offset < 0 || max <= 0)
		return;
	xlua_dref * r = (xlua_dref *) ref;
	assert(r->m_ours);
	xlua_data_mutex.lock();
	if(static_cast<size_t>(offset) >= r->m_array_storage.size()){
		xlua_data_mutex.unlock();
		return;
	}
	size_t count = (std::min)(static_cast<size_t>(max), r->m_array_storage.size() - static_cast<size_t>(offset));
	bool changed = false;
	for(size_t i = 0; i < count; ++i)
	{
		double vv = values[i];
		if(r->m_array_storage[i + static_cast<size_t>(offset)] != vv)
		{
			r->m_array_storage[i + static_cast<size_t>(offset)] = vv;
			changed = true;
		}
	}
	if(changed) xlua_dref_changed(r);
	xlua_data_mutex.unlock();
	//if(changed && r->m_notify_func)
	//	r->m_notify_func(r,r->m_notify_ref);
	
}

static int xlua_getvf(void * ref, float * values, int offset, int max)
{
	if(!active)
		return 0;
	xlua_dref * r = (xlua_dref *) ref;
	assert(r->m_ours);
	xlua_data_mutex.lock();
	size_t count=0;
	if(values == NULL){
		count=r->m_array_storage.size();
		xlua_data_mutex.unlock();
		return (int)count;
	}
	if(offset < 0 || max <= 0 || static_cast<size_t>(offset) >= r->m_array_storage.size()){
		xlua_data_mutex.unlock();
		return 0;
	}
	count = (std::min)(static_cast<size_t>(max), r->m_array_storage.size() - static_cast<size_t>(offset));
	for(size_t i = 0; i < count; ++i)
		values[i] = static_cast<float>(r->m_array_storage[i + static_cast<size_t>(offset)]);// r->m_array_storage[i + offset];
	xlua_data_mutex.unlock();
	return (int)count;
}

static void xlua_setvf(void * ref, float * values, int offset, int max)
{
	if(!active)
		return;
	if(values == NULL || offset < 0 || max <= 0)
		return;
	xlua_dref * r = (xlua_dref *) ref;
	assert(r->m_ours);
	xlua_data_mutex.lock();
	if(static_cast<size_t>(offset) >= r->m_array_storage.size()){
		xlua_data_mutex.unlock();
		return;
	}
	bool changed = false;
	size_t count = (std::min)(static_cast<size_t>(max), r->m_array_storage.size() - static_cast<size_t>(offset));
	for(size_t i = 0; i < count; ++i)
	{
		double vv = values[i];
		if(r->m_array_storage[i + static_cast<size_t>(offset)] != vv)
		{
			r->m_array_storage[i + static_cast<size_t>(offset)] = vv;
			changed = true;
		}
	}
	if(changed) xlua_dref_changed(r);
	xlua_data_mutex.unlock();
	//if(changed && r->m_notify_func)
	//	r->m_notify_func(r,r->m_notify_ref);
	
}

// For strings
static int xlua_getvb(void * ref, void * values, int offset, int max)
{
	if(!active)
		return 0;
	char * dst = (char *) values;
	xlua_dref * r = (xlua_dref *) ref;
	assert(r->m_ours);
	xlua_data_mutex.lock();
	size_t count = 0;
	if(values == NULL){
		count=r->m_string_storage.size();
		xlua_data_mutex.unlock();
		return (int)count;
	}
	if(offset < 0 || max <= 0 || static_cast<size_t>(offset) >= r->m_string_storage.size()){
		xlua_data_mutex.unlock();
		return 0;
	}
	count = (std::min)(static_cast<size_t>(max), r->m_string_storage.size() - static_cast<size_t>(offset));
	for(size_t i = 0; i < count; ++i)
		dst[i] = r->m_string_storage[i + static_cast<size_t>(offset)];
	xlua_data_mutex.unlock();
	return (int)count;
}

static void xlua_setvb(void * ref, void * values, int offset, int max)
{
	if(!active)
		return;
	if(values == NULL || offset < 0 || max <= 0)
		return;
	const char * src = (const char *) values;
	xlua_dref * r = (xlua_dref *) ref;
	assert(r->m_ours);
	std::lock_guard<std::mutex> lock(xlua_data_mutex);
	size_t start = static_cast<size_t>(offset);
	size_t count = static_cast<size_t>(max);
	if(start > r->m_string_storage.max_size() || count > r->m_string_storage.max_size() - start){
		return;
	}
	size_t new_len = start + count;
	const bool changed = new_len > r->m_string_storage.size() ||
		r->m_string_storage.compare(start, count, src, count) != 0;
	if(new_len > r->m_string_storage.size())
		r->m_string_storage.resize(new_len);
	std::copy_n(src, count, r->m_string_storage.begin() + start);
	if(changed)
		xlua_dref_changed(r);
	//if(r->m_notify_func && changed)
	//	r->m_notify_func(r,r->m_notify_ref);
	
}
void	grabLocal(xtlua_dref * who){
	xlua_dref * f;
	for(f = l_drefs; f; f = f->m_next)
	if(f->m_name == who->m_name&&f->m_ours==1)
	{
		who->local_dref=f;
		who->m_index=f->m_index;
		who->m_types=f->m_types;
		who->m_ours=1;
		return;
		
	}

}

static void resolve_lua_dref(xlua_dref * d)
{
	assert(d->m_dref == NULL);
	assert(d->m_types == 0);
	assert(d->m_index == -1);
	assert(d->m_ours == 0);
	d->m_dref = XPLMFindDataRef(d->m_name.c_str());
	if(d->m_dref)
	{
		d->m_index = -1;
		d->m_types = XPLMGetDataRefTypes(d->m_dref);
	}
	else
	{
		string::size_type obrace = d->m_name.find('[');
		string::size_type cbrace = d->m_name.find(']');
		if(obrace != d->m_name.npos && cbrace != d->m_name.npos)			// Gotta have found the braces
		if(obrace > 0)														// dref name can't be empty
		if(cbrace > obrace)													// braces must be in-order - avoids unsigned math insanity
		if((cbrace - obrace) > 1)											// Gotta have SOMETHING in between the braces
		{
			string number = d->m_name.substr(obrace+1,cbrace - obrace - 1);
			string refname = d->m_name.substr(0,obrace);
			
			XPLMDataRef arr = XPLMFindDataRef(refname.c_str());				// Only if we have a valid name
			if(arr)
			{
				printf("found arr dref %s",d->m_name.c_str());
				XPLMDataTypeID tid = XPLMGetDataRefTypes(arr);
				if(tid & (xplmType_FloatArray | xplmType_IntArray))			// AND are array type
				{
					int idx = atoi(number.c_str());							// AND have a non-negetive index
					if(idx >= 0)
					{
						d->m_dref = arr;									// Now we know we're good, record all of our info
						d->m_types = tid;
						d->m_index = idx;
					}
				}
			}
		}
	}
}

static void resolve_dref(xtlua_dref * d)
{
	xtluaDefs.XTqueueresolve_dref(d);
}
static void resolve_xp_dref(xlua_dref * d)
{
	assert(d->m_dref == NULL);
	assert(d->m_types == 0);
	assert(d->m_index == -1);
	assert(d->m_ours == 0);
	d->m_dref = XPLMFindDataRef(d->m_name.c_str());
	if(d->m_dref)
	{
		d->m_index = -1;
		d->m_types = XPLMGetDataRefTypes(d->m_dref);
	}
	else
	{
		string::size_type obrace = d->m_name.find('[');
		string::size_type cbrace = d->m_name.find(']');
		if(obrace != d->m_name.npos && cbrace != d->m_name.npos)			// Gotta have found the braces
		if(obrace > 0)														// dref name can't be empty
		if(cbrace > obrace)													// braces must be in-order - avoids unsigned math insanity
		if((cbrace - obrace) > 1)											// Gotta have SOMETHING in between the braces
		{
			string number = d->m_name.substr(obrace+1,cbrace - obrace - 1);
			string refname = d->m_name.substr(0,obrace);
			
			XPLMDataRef arr = XPLMFindDataRef(refname.c_str());				// Only if we have a valid name
			if(arr)
			{
				XPLMDataTypeID tid = XPLMGetDataRefTypes(arr);
				if(tid & (xplmType_FloatArray | xplmType_IntArray))			// AND are array type
				{
					int idx = atoi(number.c_str());							// AND have a non-negetive index
					if(idx >= 0)
					{
						d->m_dref = arr;									// Now we know we're good, record all of our info
						d->m_types = tid;
						d->m_index = idx;
					}
				}
			}
		}
	}
}
//moved to xpmtdatatypes.cpp
/*static void do_resolve_dref(xtlua_dref * d)
{
	assert(d->m_dref == NULL);
	assert(d->m_types == 0);
	assert(d->m_index == -1);
	assert(d->m_ours == 0);
	d->m_dref = XPLMFindDataRef(d->m_name.c_str());
	if(d->m_dref)
	{
		d->m_index = -1;
		d->m_types = XPLMGetDataRefTypes(d->m_dref);
	}
	else
	{
		string::size_type obrace = d->m_name.find('[');
		string::size_type cbrace = d->m_name.find(']');
		if(obrace != d->m_name.npos && cbrace != d->m_name.npos)			// Gotta have found the braces
		if(obrace > 0)														// dref name can't be empty
		if(cbrace > obrace)													// braces must be in-order - avoids unsigned math insanity
		if((cbrace - obrace) > 1)											// Gotta have SOMETHING in between the braces
		{
			string number = d->m_name.substr(obrace+1,cbrace - obrace - 1);
			string refname = d->m_name.substr(0,obrace);
			
			XPLMDataRef arr = XPLMFindDataRef(refname.c_str());				// Only if we have a valid name
			if(arr)
			{
				XPLMDataTypeID tid = XPLMGetDataRefTypes(arr);
				if(tid & (xplmType_FloatArray | xplmType_IntArray))			// AND are array type
				{
					int idx = atoi(number.c_str());							// AND have a non-negetive index
					if(idx >= 0)
					{
						d->m_dref = arr;									// Now we know we're good, record all of our info
						d->m_types = tid;
						d->m_index = idx;
					}
				}
			}
		}
	}
}*/

void			xlua_validate_drefs()
{
	for(xtlua_dref * f : worker_drefs_snapshot())
	{
	#if MOBILE
		assert(f->m_dref != NULL);
	#else
		if(f->m_dref == NULL)
			printf("WARNING: xtlua dataref %s is used but not defined.\n", f->m_name.c_str());
	#endif


	}
	for(xlua_dref * f = l_drefs; f; f = f->m_next)
	{
	#if MOBILE
		assert(f->m_dref != NULL);
	#else
		if(f->m_dref == NULL)
			printf("WARNING: xlua dataref %s is used but not defined.\n", f->m_name.c_str());
	#endif
	}
	xtluaDefs.refreshAllDataRefs();


}

xlua_dref *		xlua_find_dref(const char * name)
{
	for(xlua_dref * f = l_drefs; f; f = f->m_next)
	if(f->m_name == name)
	{
		TRACE_DATAREFS("Found %s as %p\n", name,f);
		return f;
	}
	// We have never tried to find this dref before - make a new record
	xlua_dref * d = new xlua_dref;
	d->m_next = l_drefs;
	l_drefs = d;
	d->m_name = name;
	d->m_dref = NULL;
	d->m_index = -1;
	d->m_types = 0;
	d->m_ours = 0;
	d->m_notify_func = NULL;
	d->m_notify_ref = NULL;
	d->m_number_storage = 0;
	
	resolve_lua_dref(d);

	TRACE_DATAREFS("Speculating %s as %p\n", name,d);

	return d;
}
xtlua_dref *		xtlua_find_dref(const char * name)
{
	// Only immutable list nodes are published here. Resolution uses its own
	// queue and release-publishes binding metadata later on the main thread.
	std::lock_guard<std::mutex> lock(worker_dref_list_mutex);
	for(xtlua_dref * f = s_drefs; f; f = f->m_next)
	if(f->m_name == name)
	{
		TRACE_DATAREFS("Found %s as %p\n", name,f);
		return f;
	}
	// We have never tried to find this dref before - make a new record
	xtlua_dref * d = new xtlua_dref;
	d->m_next = s_drefs;
	s_drefs = d;
	d->m_name = name;
	d->m_dref = NULL;
	d->m_index = -1;
	d->m_types = 0;
	d->m_ours = 0;
	//d->m_notify_func = NULL;
	//d->m_notify_ref = NULL;
	//d->m_number_storage = 0;
	
	resolve_dref(d);

	TRACE_DATAREFS("Speculating %s as %p\n", name,d);

	return d;
}
xlua_dref *		xlua_create_dref(const char * name, xtlua_dref_type type, int dim, int writable, xlua_dref_notify_f func, void * ref)
{
	//printf("ERROR: xTLua cannot create datarefs - us xLua.\n");
	//return NULL;
	
	assert(type != xlua_none);
	assert(name);
	assert(type != xlua_array || dim > 0);
	assert(writable || func == NULL);
	
	string n(name);
	xlua_dref * f;
	for(f = l_drefs; f; f = f->m_next)
	if(f->m_name == n)
	{
		if(f->m_ours || f->m_dref)
		{
			printf("ERROR: %s is already a dataref.\n",name);
			return NULL;
		}
		TRACE_DATAREFS("Reusing %s as %p\n", name,f);		
		break;
	}
	
	if(n.find('[') != n.npos)
	{
		printf("ERROR: %s contains brackets in its name.\n", name);
		return NULL;
	}
	
	XPLMDataRef other = XPLMFindDataRef(name);
	if(other && XPLMIsDataRefGood(other))
	{
		printf("ERROR: %s is used by another plugin.\n", name);
		return NULL;
	}
	
	xlua_dref * d = f;
	if(!d)
	{
		d = new xlua_dref;
		d->m_next = l_drefs;
		l_drefs = d;
		printf("Creating %s as %p\n", name,d);		
	}
	d->m_name = name;
	d->m_index = -1;
	d->m_ours = 1;
	d->m_notify_func = func;
	d->m_notify_ref = ref;
	d->m_number_storage = 0;

	switch(type) {
	case xlua_number:
		d->m_types = xplmType_Int|xplmType_Float|xplmType_Double;
		d->m_dref = XPLMRegisterDataAccessor(name, d->m_types, writable,
						xlua_geti, xlua_seti,
						xlua_getf, xlua_setf,
						xlua_getd, xlua_setd,
						NULL, NULL,
						NULL, NULL,
						NULL, NULL,
						d, d);
		break;
	case xlua_array:
		//printf("create array %s %d",name,dim);
		d->m_types = xplmType_FloatArray|xplmType_IntArray;
		d->m_dref = XPLMRegisterDataAccessor(name, d->m_types, writable,
						NULL, NULL,
						NULL, NULL,
						NULL, NULL,
						xlua_getvi, xlua_setvi,
						xlua_getvf, xlua_setvf,
						NULL, NULL,
						d, d);
		d->m_array_storage.resize(dim);
		break;
	case xlua_string:
		d->m_types = xplmType_Data;
		d->m_dref = XPLMRegisterDataAccessor(name, d->m_types, writable,
						NULL, NULL,
						NULL, NULL,
						NULL, NULL,
						NULL, NULL,
						NULL, NULL,
						xlua_getvb, xlua_setvb,
						d, d);
		break;
	case xlua_none:
		break;
	}

	return d;
}

xtlua_dref_type	xtlua_dref_get_type(xtlua_dref * who)
{
	if(!who || !who->m_resolved.load(std::memory_order_acquire))
		return xlua_none;
	if(who->m_types & xplmType_Data)
		return xlua_string;
	if(who->m_index >= 0)
		return  xlua_number;
	if(who->m_types & (xplmType_FloatArray|xplmType_IntArray))
		return xlua_array;
	if(who->m_types & (xplmType_Int|xplmType_Float|xplmType_Double))
		return xlua_number;
	return xlua_none;
}
xtlua_dref_type	xlua_dref_get_type(xlua_dref * who)
{
	if(who->m_types & xplmType_Data)
		return xlua_string;
	if(who->m_index >= 0)
		return  xlua_number;
	if(who->m_types & (xplmType_FloatArray|xplmType_IntArray))
		return xlua_array;
	if(who->m_types & (xplmType_Int|xplmType_Float|xplmType_Double))
		return xlua_number;
	return xlua_none;
}
void			xtlua_dref_preUpdate(){
	std::unordered_set<xlua_dref *> changes;
	{
		std::lock_guard<std::mutex> lock(xlua_change_mutex);
		changes.swap(changedDrefs);
	}
	for ( xlua_dref * r: changes) {
		if(r->m_notify_func)
			r->m_notify_func(r,r->m_notify_ref);
	}
	
}
// meat of setting drefs in here - lock the lua thread and update all datarefs to/from X-Plane
void			xtlua_dref_postUpdate(){

	//xtluaDefs.ShowDataRefs();
	xtluaDefs.updateDataRefs();

}
int	xtlua_dref_get_dim(xtlua_dref * who)
{
	if(!who || !who->m_resolved.load(std::memory_order_acquire))
		return 0;
	//if(who->m_ours)
	//	return who->m_array_storage.size();
	if(who->m_types & xplmType_Data)
		return 0;
	if(who->m_index >= 0)
		return  1;
	if(who->m_types & xplmType_FloatArray||who->m_types & xplmType_IntArray)
	{
		return xtluaDefs.XTGetDatavf(who, NULL, 0, 0,who->m_ours);
	}
	/*if(who->m_types & xplmType_IntArray)
	{
		return xtluaDefs.XTGetDatavi(who->m_dref, NULL, 0, 0,who->m_ours);
	}*/
	if(who->m_types & (xplmType_Int|xplmType_Float|xplmType_Double))
		return 1;
	return 0;
}
void	xlua_dref_ours(xlua_dref * who)
{
	assert(who->m_ours==1);
}
int	xlua_dref_get_dim(xlua_dref * who)
{
	if(who->m_ours){
		std::lock_guard<std::mutex> lock(xlua_data_mutex);
		return static_cast<int>(who->m_array_storage.size());
	}
	if(who->m_types & xplmType_Data)
		return 0;
	if(who->m_index >= 0)
		return 1;
	if(who->m_types & xplmType_FloatArray)
		return XPLMGetDatavf(who->m_dref, NULL, 0, 0);
	if(who->m_types & xplmType_IntArray)
		return XPLMGetDatavi(who->m_dref, NULL, 0, 0);
	if(who->m_types & (xplmType_Int|xplmType_Float|xplmType_Double))
		return 1;
	return 0;
}
double			xlua_dref_get_number(xlua_dref * d)
{
	if(d->m_ours){
		std::lock_guard<std::mutex> lock(xlua_data_mutex);
		return d->m_number_storage;
	}
	if(d->m_index >= 0)
	{
		if(d->m_types & xplmType_FloatArray)
		{
			float r;
			if(XPLMGetDatavf(d->m_dref, &r, d->m_index, 1))
				return r;
			return 0.0;
		}
		else if(d->m_types & xplmType_IntArray)
		{
			int r;
			if(XPLMGetDatavi(d->m_dref, &r, d->m_index, 1))
				return r;
			return 0.0;
		}
		return 0.0;
	}
	if(d->m_types & xplmType_Double)
		return XPLMGetDatad(d->m_dref);
	if(d->m_types & xplmType_Float)
		return XPLMGetDataf(d->m_dref);
	if(d->m_types & xplmType_Int)
		return XPLMGetDatai(d->m_dref);
	return 0.0;
}

void			xlua_dref_set_number(xlua_dref * d, double value)
{
	if(d->m_ours)
	{
		xlua_data_mutex.lock();
		if(value!=d->m_number_storage)
			xlua_dref_changed(d);
		d->m_number_storage = value;
		xlua_data_mutex.unlock();
		return;
	}

	if(d->m_index >= 0)
	{
		if(d->m_types & xplmType_FloatArray)
		{
			float r = static_cast<float>(value);
			XPLMSetDatavf(d->m_dref, &r, d->m_index, 1);
		}
		else if(d->m_types & xplmType_IntArray)
		{
			int r = xlua_round_to_int(value);
			XPLMSetDatavi(d->m_dref, &r, d->m_index, 1);
		}
		return;
	}
	if(d->m_types & xplmType_Double)
	{
		XPLMSetDatad(d->m_dref, value);
	}
	else if(d->m_types & xplmType_Float)
	{
		float v = static_cast<float>(value);
		XPLMSetDataf(d->m_dref, v);
	}
	else if(d->m_types & xplmType_Int)
	{
		int r = xlua_round_to_int(value);
		XPLMSetDatai(d->m_dref, r);
	}
}
double			xlua_dref_get_array(xlua_dref * d, int n)
{
	if(n < 0)
		return 0.0;
	assert(n >= 0);
	if(d->m_ours)
	{
		xlua_data_mutex.lock();
		double retVal=0.0;
		if(n < (int)d->m_array_storage.size())
			retVal=d->m_array_storage[n];
		//printf("get array %d=%f\n",n,retVal);	
		xlua_data_mutex.unlock();
		return retVal;
	}
	if(d->m_types & xplmType_FloatArray)
	{
		float r;
		if(XPLMGetDatavf(d->m_dref, &r, n, 1))
			return r;
		return 0.0;
	}
	if(d->m_types & xplmType_IntArray)
	{
		int r;
		if(XPLMGetDatavi(d->m_dref, &r, n, 1))
			return r;
		return 0.0;
	}
	return 0.0;
}

void			xlua_dref_set_array(xlua_dref * d, int n, double value)
{
	if(n < 0)
		return;
	assert(n >= 0);
	if(d->m_ours)
	{
		xlua_data_mutex.lock();
		if(n < (int)d->m_array_storage.size()){
			if(value!=d->m_array_storage[n])
				xlua_dref_changed(d);
			//printf("set array %d=%f\n",n,value);	
			d->m_array_storage[n] = value;
		}
		xlua_data_mutex.unlock();
		return;
	}
	if(d->m_types & xplmType_FloatArray)
	{
		float r = static_cast<float>(value);
		XPLMSetDatavf(d->m_dref, &r, n, 1);
	}
	else if(d->m_types & xplmType_IntArray)
	{
		int r = xlua_round_to_int(value);
		XPLMSetDatavi(d->m_dref, &r, n, 1);
	}
}

std::vector<double> xlua_dref_get_array_values(xlua_dref * d, int offset, int count)
{
	if(!d || offset < 0 || count < 0 || d->m_index >= 0 ||
		!(d->m_types & (xplmType_FloatArray | xplmType_IntArray)))
		return {};
	const size_t start = static_cast<size_t>(offset);
	const size_t length = static_cast<size_t>(count);
	if(d->m_ours) {
		std::lock_guard<std::mutex> lock(xlua_data_mutex);
		if(start > d->m_array_storage.size() || length > d->m_array_storage.size() - start)
			return {};
		return std::vector<double>(d->m_array_storage.begin() + start,
								   d->m_array_storage.begin() + start + length);
	}
	// Main-thread path: one SDK buffer transfer, never one callback per element.
	// A provider may shrink during its own callback; expose only a complete range.
	const bool floats = (d->m_types & xplmType_FloatArray) != 0;
	const int size = floats ? XPLMGetDatavf(d->m_dref, nullptr, 0, 0)
		: XPLMGetDatavi(d->m_dref, nullptr, 0, 0);
	if(size < 0 || offset > size || count > size - offset) return {};
	if(count == 0) return {};
	std::vector<double> result(length);
	if(floats) {
		std::vector<float> snapshot(length);
		if(XPLMGetDatavf(d->m_dref, snapshot.data(), offset, count) != count)
			return {};
		std::copy(snapshot.begin(), snapshot.end(), result.begin());
	} else {
		std::vector<int> snapshot(length);
		if(XPLMGetDatavi(d->m_dref, snapshot.data(), offset, count) != count)
			return {};
		std::copy(snapshot.begin(), snapshot.end(), result.begin());
	}
	return result;
}

int xlua_dref_copy_owned_array(xlua_dref * d, double * values, int offset, int count)
{
	if(!d || !d->m_ours || offset < 0 || count < 0) return 0;
	std::lock_guard<std::mutex> lock(xlua_data_mutex);
	if(!values) return static_cast<int>(d->m_array_storage.size());
	const size_t start = static_cast<size_t>(offset);
	if(start >= d->m_array_storage.size()) return 0;
	const size_t copied = (std::min)(static_cast<size_t>(count), d->m_array_storage.size() - start);
	std::copy_n(d->m_array_storage.data() + start, copied, values);
	return static_cast<int>(copied);
}

int xlua_dref_set_array_values(xlua_dref * d, const std::vector<double>& values, int offset)
{
	if(!d || offset < 0 || d->m_index >= 0 ||
		!(d->m_types & (xplmType_FloatArray | xplmType_IntArray)) ||
		values.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
		return -1;
	const size_t start = static_cast<size_t>(offset);
	if(d->m_ours) {
		std::lock_guard<std::mutex> lock(xlua_data_mutex);
		if(start > d->m_array_storage.size() || values.size() > d->m_array_storage.size() - start)
			return -1;
		bool changed = false;
		for(size_t i = 0; i < values.size(); ++i) {
			if(d->m_array_storage[start + i] != values[i]) {
				d->m_array_storage[start + i] = values[i];
				changed = true;
			}
		}
		if(changed) xlua_dref_changed(d);
		return static_cast<int>(values.size());
	}
	// Main-thread SDK writes cannot hold the owned-storage or bridge mutex.
	const bool floats = (d->m_types & xplmType_FloatArray) != 0;
	const int size = floats ? XPLMGetDatavf(d->m_dref, nullptr, 0, 0)
		: XPLMGetDatavi(d->m_dref, nullptr, 0, 0);
	if(size < 0 || offset > size || values.size() > static_cast<size_t>(size - offset))
		return -1;
	const int count = static_cast<int>(values.size());
	if(count == 0) return 0;
	if(floats) {
		std::vector<float> writeBuffer(values.begin(), values.end());
		XPLMSetDatavf(d->m_dref, writeBuffer.data(), offset, count);
	} else {
		std::vector<int> writeBuffer;
		writeBuffer.reserve(values.size());
		for(double value : values) writeBuffer.push_back(xlua_round_to_int(value));
		XPLMSetDatavi(d->m_dref, writeBuffer.data(), offset, count);
	}
	// SDK setters return no acknowledgement. This is the submitted count.
	return count;
}

string			xlua_dref_get_string(xlua_dref * d)
{
	if(d->m_ours){
		xlua_data_mutex.lock();
		string retVal=string(d->m_string_storage);
		xlua_data_mutex.unlock();
		return retVal;
	}
	
	if(d->m_types & xplmType_Data)
	{
		int l = XPLMGetDatab(d->m_dref, NULL, 0, 0);
		if(l > 0)
		{
			vector<char>	buf(l);
			l = XPLMGetDatab(d->m_dref, &buf[0], 0, l);
			if(l >= 0 && l <= (int)buf.size())
				return string(buf.data(), static_cast<size_t>(l));
		}
	}
	return string();
}

void			xlua_dref_set_string(xlua_dref * d, const string& value)
{
	if(d->m_ours)
	{
		xlua_data_mutex.lock();
		if(value!=d->m_string_storage)
			xlua_dref_changed(d);
		d->m_string_storage = value;
		xlua_data_mutex.unlock();
		return;
	}
	if(d->m_types & xplmType_Data)
	{
		if(value.size() <= static_cast<size_t>((std::numeric_limits<int>::max)()))
			XPLMSetDatab(d->m_dref, (void *) value.data(), 0, static_cast<int>(value.size()));
	}
}
double	xtlua_dref_get_number(xtlua_dref * d)
{
	if(!d || !d->m_resolved.load(std::memory_order_acquire))
		return 0.0;
	if(d->m_index >= 0)
	{
		if(d->m_types & (xplmType_FloatArray | xplmType_IntArray))
		{
			double r;
			if(xtluaDefs.XTGetDatavf(d, &r, d->m_index, 1,d->m_ours))
				return r;
			return 0.0;
		}

		return 0.0;
	}

	if(d->m_types & xplmType_Float||d->m_types & xplmType_Int||d->m_types & xplmType_Double)
	{
		return xtluaDefs.XTGetDataf(d,d->m_ours);
	}
	return 0.0;
}

void			xtlua_dref_set_number(xtlua_dref * d, double value)
{
	if(!d || !d->m_resolved.load(std::memory_order_acquire))
		return;
	if(d->m_index >= 0)
	{
		if(d->m_types & xplmType_FloatArray||d->m_types & xplmType_IntArray)
		{
			xtluaDefs.XTSetDatavf(d, value, d->m_index);
		}
		return;
	}

	if(d->m_types & xplmType_Float || d->m_types & xplmType_Double || d->m_types & xplmType_Int)
	{
		xtluaDefs.XTSetDataf(d, value,d->m_ours);
	}
}

double			xtlua_dref_get_array(xtlua_dref * d, int n)
{
	if(!d || !d->m_resolved.load(std::memory_order_acquire))
		return 0.0;
	if(n < 0)
		return 0.0;
	assert(n >= 0);
	if((d->m_types & xplmType_FloatArray)||(d->m_types & xplmType_IntArray))
	{
		double r;
		if(xtluaDefs.XTGetDatavf(d, &r, n, 1,d->m_ours))
			return r;
		return 0.0;
	}
	
	return 0.0;
}

void			xtlua_dref_set_array(xtlua_dref * d, int n, double value)
{
	if(!d || !d->m_resolved.load(std::memory_order_acquire))
		return;
	if(n < 0)
		return;
	assert(n >= 0);

	if((d->m_types & xplmType_FloatArray) || (d->m_types & xplmType_IntArray))
	{

		xtluaDefs.XTSetDatavf(d, value, n);
	}
	/*if(d->m_types & xplmType_IntArray)
	{
		int r = value;
		xtluaDefs.XTSetDatavi(d->m_dref, &r, n, 1,d->m_ours);
	}*/
}

std::vector<double> xtlua_dref_get_array_values(xtlua_dref * d, int offset, int count)
{
	if(!d || !d->m_resolved.load(std::memory_order_acquire) || d->m_index >= 0 || !(d->m_types & (xplmType_FloatArray | xplmType_IntArray)))
		return {};
	return xtluaDefs.XTGetArrayValues(d, offset, count);
}

int xtlua_dref_set_array_values(xtlua_dref * d, const std::vector<double>& values, int offset)
{
	if(!d || !d->m_resolved.load(std::memory_order_acquire) || d->m_index >= 0 || !(d->m_types & (xplmType_FloatArray | xplmType_IntArray)))
		return -1;
	return xtluaDefs.XTSetArrayValues(d, values, offset);
}

string xtlua_dref_get_string(xtlua_dref * d)
{
	if(!d)
		return {};
	const bool special = d->m_name.rfind("xtlua/", 0) == 0;
	if(!special && !d->m_resolved.load(std::memory_order_acquire))
		return {};
	if(special || (d->m_types & xplmType_Data))
		return xtluaDefs.XTGetString(d);
	return {};
}

void xtlua_dref_set_string(xtlua_dref * d, const string& value)
{
	if(!d)
		return;
	const bool special = d->m_name.rfind("xtlua/", 0) == 0;
	if(!special && !d->m_resolved.load(std::memory_order_acquire))
		return;
	if(special || (d->m_types & xplmType_Data))
		xtluaDefs.XTSetDatab(d, value);
}

// This attempts to re-establish the name->dref link for any unresolved drefs.  This can be used if we declare
// our dref early and then ANOTHER add-on is loaded that defines it.
void			xlua_relink_all_drefs()
{
#if !MOBILE
	XPLMPluginID dre = XPLMFindPluginBySignature(STAT_PLUGIN_SIG);
	
	if(dre != XPLM_NO_PLUGIN_ID)
	if(!XPLMIsPluginEnabled(dre))
	{
		printf("WARNING: can't register drefs - DRE is not enabled.\n");
		dre = XPLM_NO_PLUGIN_ID;
	}
#endif
	xtluaDefs.paused_ref=NULL;
	xtluaDefs.sim_time_ref=NULL;
	for(xtlua_dref * d : worker_drefs_snapshot())
	{
		if(d->m_dref == NULL)
		{
			assert(!d->m_ours);
			resolve_dref(d);
		}
#if !MOBILE
		if(d->m_ours)
		if(dre != XPLM_NO_PLUGIN_ID)
		{
			//printf("registered: %s\n", d->m_name.c_str());
			XPLMSendMessageToPlugin(dre, MSG_ADD_DATAREF, (void *)d->m_name.c_str());		
		}		
#endif
	}
	for(xlua_dref * d = l_drefs; d; d = d->m_next)
	{
		if(d->m_dref == NULL)
		{
			assert(!d->m_ours);
			resolve_xp_dref(d);
		}
#if !MOBILE
		if(d->m_ours)
		if(dre != XPLM_NO_PLUGIN_ID)
		{
			//printf("registered: %s\n", d->m_name.c_str());
			XPLMSendMessageToPlugin(dre, MSG_ADD_DATAREF, (void *)d->m_name.c_str());		
		}		
#endif
	}
	active=true;
}

std::vector<XTCmd*> runQueue;
std::vector<string> messageQueue;
std::mutex data_mutex;
static bool accepting_command_callbacks = true;
static unsigned main_command_dispatch_depth = 0; // Main thread only; blocks reentrant lifecycle work.
static std::vector<XPLMCommandRef> main_command_dispatch_stack; // Detect aliases of the same SDK command too.
// Main-thread bookkeeping: installing a later wrapper must not register an
// already installed SDK callback a second time.
static std::unordered_map<xtlua_cmd *, unsigned> registered_command_handlers;
enum class worker_handler_slot { pre, main, post };

static void enqueue_command_phase(xtlua_cmd * me, XPLMCommandPhase phase,
	worker_handler_slot slot)
{
	if(!me)
		return;
	const float now = static_cast<float>(xtluaDefs.XTGetElapsedTime());
	std::unique_ptr<XTCmd> command(new XTCmd());
	std::lock_guard<std::mutex> lock(data_mutex);
	if(!accepting_command_callbacks)
		return;
	if(phase == xplm_CommandBegin)
		me->m_down_time = now;
	if(slot == worker_handler_slot::pre)
	{
		command->runFunc = me->m_pre_handler;
		command->m_func_ref = me->m_pre_ref;
	}
	else if(slot == worker_handler_slot::main)
	{
		command->runFunc = me->m_main_handler;
		command->m_func_ref = me->m_main_ref;
	}
	else
	{
		command->runFunc = me->m_post_handler;
		command->m_func_ref = me->m_post_ref;
	}
	if(!command->runFunc)
		return;
	command->phase = phase;
	command->xluaref = me;
	command->duration = (std::max)(0.0f, now - me->m_down_time);
	// Notifications are events, not a latest-value cache. CommandOnce delivers
	// begin/end at the same timestamp. Never deduplicate, filter or drop them.
	runQueue.push_back(command.get());
	command.release();
}

static int xlua_std_pre_handler(XPLMCommandRef c, XPLMCommandPhase phase, void * ref)
{
	(void)c;
	enqueue_command_phase(static_cast<xtlua_cmd *>(ref), phase, worker_handler_slot::pre);
	return 1;
}

static int xlua_std_main_handler(XPLMCommandRef c, XPLMCommandPhase phase, void * ref)
{
	(void)c;
	enqueue_command_phase(static_cast<xtlua_cmd *>(ref), phase, worker_handler_slot::main);
	return 0;
}

static int xlua_std_post_handler(XPLMCommandRef c, XPLMCommandPhase phase, void * ref)
{
	(void)c;
	enqueue_command_phase(static_cast<xtlua_cmd *>(ref), phase, worker_handler_slot::post);
	return 1;
}
std::vector<XTCmd*> get_runQueue(){
	std::vector<XTCmd*> items;
	std::lock_guard<std::mutex> lock(data_mutex);
	items.swap(runQueue);
	return items;
}
void xtlua_localNavData(){
	xtluaDefs.update_localNavData();//runs on xtlua thread, but lat/lon come from sim
}
std::vector<string> get_runMessages(){
	std::vector<string> items;
	std::lock_guard<std::mutex> lock(data_mutex);
	items.swap(messageQueue);
	return items;
}
void xlua_add_callout(string callout){
	printf("xlua_add_callout %s\n",callout.c_str());
	xtluaDefs.refreshAllDataRefs();
	data_mutex.lock();
	messageQueue.push_back(callout);
	data_mutex.unlock();
}
void xlua_setLoadStatus(int loadStatus)
{
	xtluaDefs.isLoaded=loadStatus;
}
bool xlua_ispaused(){
	int retVal1=xtluaDefs.isPaused;
	int retVal2=xtluaDefs.isLoaded;
	return retVal1==1&&retVal2==1;
}
double xlua_get_simulated_time(){
	// Main-thread time snapshot; this never queries the SDK from the worker.
	return xtluaDefs.XTGetElapsedTime();
}
bool xtlua_is_sdk_dispatch_active(){
	return xtluaDefs.isMainDispatchActive() || xlua_is_main_timer_dispatch_active() ||
		main_command_dispatch_depth != 0;
}
int xtlua_dref_resolveDREFQueue(){
	const int resolved = xtluaDefs.resolveQueue();
	// Handler installation can arrive after a command was already resolved.
	const std::vector<xtlua_cmd *> commands = xtluaDefs.XTGetHandlers();
	for(xtlua_cmd * cmd : commands)
	{
		if(cmd == NULL || cmd->m_cmd == NULL)
			continue;
		unsigned requested = 0;
		{
			std::lock_guard<std::mutex> lock(data_mutex);
			if(cmd->m_pre_handler) requested |= 1u;
			if(cmd->m_main_handler) requested |= 2u;
			if(cmd->m_post_handler) requested |= 4u;
		}
		unsigned& installed = registered_command_handlers[cmd];
		const unsigned added = requested & ~installed;
		installed |= added;
		// No queue/worker lock can span an SDK call: it may reenter a handler.
		if(added & 1u)
			XPLMRegisterCommandHandler(cmd->m_cmd, xlua_std_pre_handler, 1, cmd);
		if(added & 2u)
			XPLMRegisterCommandHandler(cmd->m_cmd, xlua_std_main_handler, 1, cmd);
		if(added & 4u)
			XPLMRegisterCommandHandler(cmd->m_cmd, xlua_std_post_handler, 0, cmd);
	}
	return resolved + (commands.empty() ? 0 : 1);
}


void xtlua_dref_cleanup()
{
	// Lifecycle has quiesced the worker and closed every classic Lua binding
	// gate. Release SDK command/camera ownership while DataRef providers and
	// their callback storage still exist, then retire the binding nodes.
	active = false;
	xtluaDefs.cleanup();

	xtlua_dref * worker_refs;
	{
		std::lock_guard<std::mutex> lock(worker_dref_list_mutex);
		worker_refs = s_drefs;
		s_drefs = NULL;
	}
	while(worker_refs)
	{
		xtlua_dref * next = worker_refs->m_next;
		// Worker bindings borrow a provider handle, including our own local
		// providers. They never own an XPLM accessor registration.
		delete worker_refs;
		worker_refs = next;
	}
	while(l_drefs)
	{
		xlua_dref * kill = l_drefs;
		l_drefs = kill->m_next;
		// Unregister only the exact handle we created, once. Looking up a
		// provider by name here could remove a different plugin's replacement.
		if(kill->m_ours && kill->m_dref)
			XPLMUnregisterDataAccessor(kill->m_dref);
		delete kill;
	}
	{
		std::lock_guard<std::mutex> lock(xlua_change_mutex);
		changedDrefs.clear();
	}
	printf("XLua Cleanup\n");
}

//
//  merge of xpcommands.cpp
//  xlua
//
//  Created by Benjamin Supnik on 4/12/16.
//
//	Copyright 2016, Laminar Research
//	This source code is licensed under the MIT open source license.
//	See LICENSE.txt for the full terms of the license.

// xTLua
// Merged by Mark Parker on 04/23/2020


static xtlua_cmd *		s_cmds = NULL;
static xlua_cmd *		l_cmds = NULL;

static void resolve_cmd(xtlua_cmd * d)
{
	xtluaDefs.XTqueueresolve_cmd(d);
}
xtlua_cmd * xtlua_find_cmd(const char * name)
{
	{
		std::lock_guard<std::mutex> lock(data_mutex);
		accepting_command_callbacks = true;
	}
	for(xtlua_cmd * i = s_cmds; i; i = i->m_next)
	if(i->m_name == name)
		return i;
		
	/*XPLMCommandRef c = XPLMFindCommand(name);	
	if(c == NULL){
		printf("ERROR: Command %s not found\n",name);
	} return NULL;*/	
		
	xtlua_cmd * nc = new xtlua_cmd;
	nc->m_next = s_cmds;
	s_cmds = nc;
	nc->m_name = name;
	resolve_cmd(nc);
	//nc->m_cmd = c;
	return nc;
}
xlua_cmd * xlua_find_cmd(const char * name)
{
	//printf("looking for %s\n",name);
	for(xlua_cmd * i = l_cmds; i; i = i->m_next)
	if(i->m_name == name)
		return i;
		
	XPLMCommandRef c = XPLMFindCommand(name);	
	if(c == NULL){
		printf("ERROR: Command %s not found\n",name);
	} 
	if(c == NULL) return NULL;	
		
	xlua_cmd * nc = new xlua_cmd;
	nc->m_next = l_cmds;
	l_cmds = nc;
	nc->m_name = name;
	nc->m_cmd = c;
	return nc;
		

}
struct MainCommandDispatchScope {
	xlua_cmd * command;
	explicit MainCommandDispatchScope(xlua_cmd * cmd) : command(cmd)
	{
		main_command_dispatch_stack.push_back(command->m_cmd);
		++main_command_dispatch_depth;
		++command->m_dispatch_depth;
	}
	~MainCommandDispatchScope()
	{
		--command->m_dispatch_depth;
		--main_command_dispatch_depth;
		main_command_dispatch_stack.pop_back();
	}
};

bool xlua_cmd_is_dispatching(xlua_cmd * cmd)
{
	return cmd && (cmd->m_dispatch_depth != 0 ||
		std::find(main_command_dispatch_stack.begin(), main_command_dispatch_stack.end(), cmd->m_cmd) !=
		main_command_dispatch_stack.end());
}

static int xlua_main_before_handler(XPLMCommandRef, XPLMCommandPhase phase, void * ref)
{
	xlua_cmd * me = static_cast<xlua_cmd *>(ref);
	if(phase < xplm_CommandBegin || phase > xplm_CommandEnd) return 1;
	const bool recursive = xlua_cmd_is_dispatching(me);
	if(!recursive) me->m_post_snapshots[phase].valid = false;
	MainCommandDispatchScope dispatch(me);
	const float now = XPLMGetElapsedTime();
	xlua_cmd::Hold hold;
	if(phase == xplm_CommandBegin) {
		me->m_has_seen_begin = true;
		hold.serial = ++me->m_next_hold;
		if(recursive) {
			// An indirect SDK callback can reenter even though classic Lua's
			// own CommandStart/Stop/Once API rejects direct same-command calls.
			// Keep this blocked Begin until its End; never borrow a later allow.
			me->m_holds.push_back(hold);
			return 0;
		}
		const bool first = me->m_hold_count++ == 0;
		if(first) me->m_down_time = now;
		hold.notify = true;
		hold.down_time = me->m_down_time;
		hold.pre_handler = me->m_pre_handler;
		hold.pre_ref = me->m_pre_ref;
		hold.main_handler = me->m_main_handler;
		hold.main_ref = me->m_main_ref;
		hold.post_handler = me->m_post_handler;
		hold.post_ref = me->m_post_ref;
		me->m_holds.push_back(hold);
		if(first) {
			const bool allowed = !me->m_filter || me->m_filter(me, me->m_filter_ref);
			// Never retain a vector reference across Lua. A recursive SDK End
			// may have consumed this still-pending Begin, in which case both
			// are blocked instead of passing a Begin whose End was already lost.
			const auto pending = std::find_if(me->m_holds.begin(), me->m_holds.end(),
				[&hold](const xlua_cmd::Hold& entry) { return entry.serial == hold.serial; });
			if(pending == me->m_holds.end()) return 0;
			me->m_filter_allow = allowed;
			pending->allowed = allowed;
			hold.allowed = allowed;
		} else {
			hold.allowed = me->m_filter_allow;
			me->m_holds.back().allowed = hold.allowed;
		}
	} else if(phase == xplm_CommandEnd) {
		if(me->m_holds.empty()) return me->m_has_seen_begin ? 0 : 1;
		hold = me->m_holds.back();
		me->m_holds.pop_back();
		if(hold.notify && me->m_hold_count != 0) --me->m_hold_count;
	} else {
		// The SDK has no source ID for Continue. Forward it while a normal
		// admitted hold exists; a blocked recursive hold cannot unmask itself.
		const auto current = std::find_if(me->m_holds.rbegin(), me->m_holds.rend(),
			[](const xlua_cmd::Hold& entry) { return entry.notify; });
		if(current == me->m_holds.rend()) return me->m_has_seen_begin ? 0 : 1;
		hold = *current;
	}
	if(!hold.allowed) return 0;
	// A filter can already have allowed Begin while its pre wrapper is still
	// running. That is not an SDK Begin yet: a recursive End must not overtake it.
	if(phase != xplm_CommandBegin && !hold.admitted) return 0;
	if(recursive) return hold.main_handler ? 0 : 1; // Never recursively enter a Lua handler.
	const auto beginSurvives = [me, phase, &hold]() {
		return phase != xplm_CommandBegin ||
			std::any_of(me->m_holds.begin(), me->m_holds.end(),
				[&hold](const xlua_cmd::Hold& entry) { return entry.serial == hold.serial; });
	};
	const float duration = (std::max)(0.0f, now - hold.down_time);
	if(hold.pre_handler) hold.pre_handler(me, phase, duration, hold.pre_ref);
	if(!beginSurvives()) return 0;
	if(hold.main_handler) {
		hold.main_handler(me, phase, duration, hold.main_ref);
		if(!beginSurvives()) return 0;
		// Replacements stop SDK propagation, so the SDK after handler cannot
		// supply our post wrapper. Invoke the captured wrapper exactly here.
		if(hold.post_handler) hold.post_handler(me, phase, duration, hold.post_ref);
		if(!beginSurvives()) return 0;
	}
	if(phase == xplm_CommandBegin) {
		// No Lua/SDK calls follow publication. In particular, never retain a
		// pointer/reference to vector storage across the callbacks above.
		const auto pending = std::find_if(me->m_holds.begin(), me->m_holds.end(),
			[&hold](const xlua_cmd::Hold& entry) { return entry.serial == hold.serial; });
		if(pending == me->m_holds.end()) return 0;
		pending->admitted = true;
	}
	if(hold.main_handler) return 0;
	me->m_post_snapshots[phase] = {hold.post_handler, hold.post_ref, hold.down_time, true};
	return 1;
}

static int xlua_main_after_handler(XPLMCommandRef, XPLMCommandPhase phase, void * ref)
{
	xlua_cmd * me = static_cast<xlua_cmd *>(ref);
	if(phase < xplm_CommandBegin || phase > xplm_CommandEnd) return 1;
	const auto snapshot = me->m_post_snapshots[phase];
	me->m_post_snapshots[phase].valid = false;
	if(!snapshot.valid || xlua_cmd_is_dispatching(me)) return 1;
	MainCommandDispatchScope dispatch(me);
	if(snapshot.handler) {
		const float duration = (std::max)(0.0f, XPLMGetElapsedTime() - snapshot.down_time);
		snapshot.handler(me, phase, duration, snapshot.ref);
	}
	return 1;
}

static void register_main_before(xlua_cmd * cmd)
{
	if(cmd->m_before_registered) return;
	cmd->m_before_registered = true;
	XPLMRegisterCommandHandler(cmd->m_cmd, xlua_main_before_handler, 1, cmd);
}
xlua_cmd * xlua_create_cmd(const char * name, const char * desc)
{
	//printf("ERROR: xTLua cannot create command - %s - use xLua and wrap them here.\n",name);
	//return NULL;
	
	

	// Ben says: we used to try to barf on commands taken over from other plugins but
	// we can get spooked by our own shadow - if we had a command last run and the user 
	// bound it to a joystick and saved prefs then...it will ALREADY exist!  So don't
	// FTFO here.

//	if(XPLMFindCommand(name) != NULL)
//	{
//		printf("ERROR: command already in use by other plugin or X-Plane: %s\n", name);
//		return NULL;
//	}

	xlua_cmd * nc = new xlua_cmd;
	nc->m_next = l_cmds;
	l_cmds = nc;
	nc->m_name = name;
	nc->m_cmd = XPLMCreateCommand(name,desc);
	//printf("NULLCommandHandler %s\n",nc->m_name.c_str());
	
	return nc;
}

void xtlua_cmd_install_handler(xtlua_cmd * cmd, xtlua_cmd_handler_f handler, void * ref)
{
	if(!cmd)
		return;
	{
		std::lock_guard<std::mutex> lock(data_mutex);
		if(cmd->m_main_handler != NULL)
		{
			printf("ERROR: there is already a main handler installed: %s.\n", cmd->m_name.c_str());
			return;
		}
		cmd->m_main_handler = handler;
		cmd->m_main_ref = ref;
	}
	xtluaDefs.XTRegisterCommandHandler(cmd);
}
void xlua_cmd_install_handler(xlua_cmd * cmd, xlua_cmd_handler_f handler, void * ref)
{
	if(!cmd || !cmd->m_cmd || !handler) return;
	if(cmd->m_main_handler != NULL)
	{
		printf("ERROR: there is already a main handler installed: %s.\n", cmd->m_name.c_str());
		return;
	}
	cmd->m_main_handler = handler;
	cmd->m_main_ref = ref;
	register_main_before(cmd);
}

void xlua_cmd_install_pre_wrapper(xlua_cmd * cmd, xlua_cmd_handler_f handler, void * ref)
{
	if(!cmd || !cmd->m_cmd || !handler) return;
	if(cmd->m_pre_handler) {
		printf("ERROR: there is already a pre handler installed: %s.\n", cmd->m_name.c_str());
		return;
	}
	cmd->m_pre_handler = handler;
	cmd->m_pre_ref = ref;
	register_main_before(cmd);
}

void xlua_cmd_install_post_wrapper(xlua_cmd * cmd, xlua_cmd_handler_f handler, void * ref)
{
	if(!cmd || !cmd->m_cmd || !handler) return;
	if(cmd->m_post_handler) {
		printf("ERROR: there is already a post handler installed: %s.\n", cmd->m_name.c_str());
		return;
	}
	cmd->m_post_handler = handler;
	cmd->m_post_ref = ref;
	// A post-only wrapper still needs the Begin timestamp and filter state.
	register_main_before(cmd);
	cmd->m_after_registered = true;
	XPLMRegisterCommandHandler(cmd->m_cmd, xlua_main_after_handler, 0, cmd);
}

void xlua_cmd_install_filter(xlua_cmd * cmd, xlua_cmd_filter_f filter, void * ref)
{
	if(!cmd || !cmd->m_cmd || !filter) return;
	if(cmd->m_filter) {
		printf("ERROR: there is already a command filter installed: %s.\n", cmd->m_name.c_str());
		return;
	}
	cmd->m_filter = filter;
	cmd->m_filter_ref = ref;
	register_main_before(cmd);
}

void xtlua_cmd_install_pre_wrapper(xtlua_cmd * cmd, xtlua_cmd_handler_f handler, void * ref)
{
	if(!cmd)
		return;
	{
		std::lock_guard<std::mutex> lock(data_mutex);
		if(cmd->m_pre_handler != NULL)
		{
			printf("ERROR: there is already a pre handler installed: %s.\n", cmd->m_name.c_str());
			return;
		}
		cmd->m_pre_handler = handler;
		cmd->m_pre_ref = ref;
	}
	xtluaDefs.XTRegisterCommandHandler(cmd);
}

void xtlua_cmd_install_post_wrapper(xtlua_cmd * cmd, xtlua_cmd_handler_f handler, void * ref)
{
	if(!cmd)
		return;
	{
		std::lock_guard<std::mutex> lock(data_mutex);
		if(cmd->m_post_handler != NULL)
		{
			printf("ERROR: there is already a post handler installed: %s.\n", cmd->m_name.c_str());
			return;
		}
		cmd->m_post_handler = handler;
		cmd->m_post_ref = ref;
	}
	xtluaDefs.XTRegisterCommandHandler(cmd); 
}

void xtlua_cmd_start(xtlua_cmd * cmd)
{
	xtluaDefs.XTCommandBegin(cmd);
}
void xtlua_cmd_stop(xtlua_cmd * cmd)
{
	xtluaDefs.XTCommandEnd(cmd);
}

void xtlua_cmd_once(xtlua_cmd * cmd)
{
	xtluaDefs.XTCommandOnce(cmd);
}
void xlua_cmd_start(xlua_cmd * cmd)
{
	if(!cmd || !cmd->m_cmd || xlua_cmd_is_dispatching(cmd)) return;
	//xtluaDefs.XTCommandBegin(cmd);
	XPLMCommandBegin(cmd->m_cmd);
}
void xlua_cmd_stop(xlua_cmd * cmd)
{
	if(!cmd || !cmd->m_cmd || xlua_cmd_is_dispatching(cmd)) return;
	//xtluaDefs.XTCommandEnd(cmd);
	XPLMCommandEnd(cmd->m_cmd);
}

void xlua_cmd_once(xlua_cmd * cmd)
{
	if(!cmd || !cmd->m_cmd || xlua_cmd_is_dispatching(cmd)) return;
	//xtluaDefs.XTCommandOnce(cmd);
	XPLMCommandOnce(cmd->m_cmd);
}
void xtlua_cmd_cleanup()
{
	// Lifecycle caller has joined/paused the worker, including its detached
	// callback batch. Close ingress before unregistering SDK handlers.
	{
		std::lock_guard<std::mutex> lock(data_mutex);
		accepting_command_callbacks = false;
		for(XTCmd * item : runQueue)
			delete item;
		runQueue.clear();
		messageQueue.clear();
	}
	while(s_cmds)
	{
		xtlua_cmd * k = s_cmds;
		const unsigned installed = registered_command_handlers[k];
		if(k->m_cmd && (installed & 1u))
			XPLMUnregisterCommandHandler(k->m_cmd, xlua_std_pre_handler, 1, k);
		if(k->m_cmd && (installed & 2u))
			XPLMUnregisterCommandHandler(k->m_cmd, xlua_std_main_handler, 1, k);
		if(k->m_cmd && (installed & 4u))
			XPLMUnregisterCommandHandler(k->m_cmd, xlua_std_post_handler, 0, k);
		s_cmds = s_cmds->m_next;
		delete k;
	}
	while(l_cmds)
	{
		xlua_cmd * k = l_cmds;
		if(k->m_cmd && k->m_before_registered)
			XPLMUnregisterCommandHandler(k->m_cmd, xlua_main_before_handler, 1, k);
		if(k->m_cmd && k->m_after_registered)
			XPLMUnregisterCommandHandler(k->m_cmd, xlua_main_after_handler, 0, k);
		l_cmds = l_cmds->m_next;
		delete k;
	}
	s_cmds = NULL;
	l_cmds = NULL;
	registered_command_handlers.clear();
}
