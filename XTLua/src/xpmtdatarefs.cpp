//
//  xpmtdatarefs.cpp
//  xTLua
//
//  Created by Mark Parker on 04/19/2020
//
//	Copyright 2020
//	This source code is licensed under the MIT open source license.
//	See LICENSE.txt for the full terms of the license.

#include <XPLMDataAccess.h>
#include <XPLMNavigation.h>
#include "xpmtdatarefs.h"
#include "shared_xpfuncs.h"
#include "XPLMCamera.h"
#include <stdio.h>
#include <assert.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include "SerialWidget.h"
#include "json/json.hpp"
#include <vector>
using nlohmann::json;
static std::mutex data_mutex;
typedef void (*XPLMLoadFMSFlightPlan_f)(int inDevice, const char * inBuffer, unsigned int inBufferLen);

namespace {
struct MainDispatchScope {
    unsigned& depth;
    bool& active;
    MainDispatchScope(unsigned& depthIn, bool& activeIn) : depth(depthIn), active(activeIn)
    { ++depth; active = true; }
    ~MainDispatchScope() { active = false; --depth; }
};

std::string bridge_key(XPLMDataRef ref)
{
    char key[32] = {};
    snprintf(key, sizeof(key), "%p", ref);
    return key;
}

std::string read_string_snapshot(XPLMDataRef ref)
{
    const int size = XPLMGetDatab(ref, nullptr, 0, 0);
    if(size <= 0)
        return {};
    std::string bytes(static_cast<size_t>(size), '\0');
    const int copied = XPLMGetDatab(ref, &bytes[0], 0, size);
    bytes.resize(static_cast<size_t>((std::max)(0, (std::min)(copied, size))));
    return bytes;
}

double read_scalar_snapshot(XPLMDataRef ref, int type)
{
    if(type == xplmType_Double) return XPLMGetDatad(ref);
    if(type == xplmType_Int) return XPLMGetDatai(ref);
    return XPLMGetDataf(ref);
}
}

static int xtlua_array_to_int(double value)
{
    if(std::isnan(value))
        return 0;
    const double rounded = std::round(value);
    if(rounded >= static_cast<double>((std::numeric_limits<int>::max)()))
        return (std::numeric_limits<int>::max)();
    if(rounded <= static_cast<double>((std::numeric_limits<int>::min)()))
        return (std::numeric_limits<int>::min)();
    return static_cast<int>(rounded);
}

void XTLuaDataRefs::XTCommandBegin(xtlua_cmd * cmd){
    if(!cmd) return;
    std::lock_guard<std::mutex> lock(data_mutex);
    if(acceptingRequests) commandQueue.push_back({cmd->m_name, CommandAction::begin});
}

void XTLuaDataRefs::XTCommandEnd(xtlua_cmd * cmd){
    if(!cmd) return;
    std::lock_guard<std::mutex> lock(data_mutex);
    if(acceptingRequests) commandQueue.push_back({cmd->m_name, CommandAction::end});
}

void XTLuaDataRefs::XTCommandOnce(xtlua_cmd * cmd){
    if(!cmd) return;
    std::lock_guard<std::mutex> lock(data_mutex);
    if(acceptingRequests) commandQueue.push_back({cmd->m_name, CommandAction::once});
}


void XTLuaDataRefs::XTRegisterCommandHandler(xtlua_cmd * cmd){
    char namec[32]={0};
    sprintf(namec,"%p",cmd);
    std::string name=namec;
    //use pointer name to only register each handler once
    printf("Will register command handlers for %s\n",cmd->m_name.c_str());
    data_mutex.lock(); 
    cmdHandlerResolveQueue[name]=cmd;
    data_mutex.unlock();

}
/*float XTLuaDataRefs::getsim_period(){
    return 0.02;	   
}*/
//begin DataRefs section
void XTLuaDataRefs::updateStringDataRefs(){
    if(mainDispatchDepth == 0) updateStringDataRefsImpl();
}
void XTLuaDataRefs::updateStringDataRefsImpl(){
    if(updatingStrings || cleaning) return;
    MainDispatchScope dispatch(mainDispatchDepth, updatingStrings);
    struct Transfer {
        std::string name;
        XPLMDataRef ref;
        std::uint64_t version;
        bool write;
        std::string bytes;
    };
    std::vector<Transfer> batch;
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        for(const auto& entry : stringdataRefs) {
            XTLuaCharArray* val = entry.second;
            if(!val || (!val->get && !val->set)) continue;
            batch.push_back({entry.first, val->ref, val->version, val->set,
                             val->set ? val->value : std::string{}});
            val->get = false; // A later worker read must survive this batch.
        }
    }
    for(Transfer& item : batch) {
        if(item.write) {
            // xplmType_Data is an exact byte buffer. In particular, forward a
            // zero-byte write; whether this truncates is the provider contract.
            // Never silently append NUL bytes or overwrite a binary tail.
            XPLMSetDatab(item.ref, const_cast<char*>(item.bytes.c_str()), 0,
                        static_cast<int>(item.bytes.size()));
        }
        std::string snapshot = read_string_snapshot(item.ref);
        std::lock_guard<std::mutex> lock(data_mutex);
        auto found = stringdataRefs.find(item.name);
        if(found == stringdataRefs.end()) continue;
        XTLuaCharArray* val = found->second;
        if(val->ref != item.ref || val->version != item.version) continue;
        if(item.write) val->set = false;
        if(!val->set) val->value = std::move(snapshot);
    }
}
float scale(XTControlObject* c){
    float x=XPLMGetDataf(c->srcDref);
    float scale=c->scale;
    if(c->scaleDref){
        scale=XPLMGetDataf(c->scaleDref);
        c->scale=scale;
    }  
    if (x < c->minin)
        return c->minout*scale;
    if (x > c->maxin)
        return c->maxout*scale;
     
     
    return (c->minout + (c->maxout - c->minout) * (x - c->minin) / (c->maxin - c->minin))*scale;

}
void XTLuaDataRefs::updateCommands(){
    if(mainDispatchDepth == 0) updateCommandsImpl();
}
void XTLuaDataRefs::updateCommandsImpl(){
    if(updatingCommands || cleaning) return;
    MainDispatchScope dispatch(mainDispatchDepth, updatingCommands);
    std::deque<CommandRequest> batch;
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        batch.swap(commandQueue);
    }
    std::deque<CommandRequest> pending;
    std::unordered_set<std::string> blockedNames;
    while(!batch.empty()) {
        CommandRequest request=std::move(batch.front());
        batch.pop_front();
        if(blockedNames.find(request.name) != blockedNames.end()) {
            pending.push_back(std::move(request));
            continue;
        }
        const XPLMCommandRef command = XPLMFindCommand(request.name.c_str());
        if(!command) {
            // Preserve FIFO for this command, while allowing an unrelated
            // held command to receive its End. No unresolved phase is dropped.
            blockedNames.insert(request.name);
            if(unresolvedCommands.insert(request.name).second)
                xtlua_queue_log("XTLua: waiting for command " + request.name + "\n");
            pending.push_back(std::move(request));
            continue;
        }
        unresolvedCommands.erase(request.name);
        switch(request.action) {
        case CommandAction::begin:
            XPLMCommandBegin(command);
            ++heldCommands[command];
            break;
        case CommandAction::end: {
            auto held=heldCommands.find(command);
            if(held != heldCommands.end()) {
                XPLMCommandEnd(command);
                if(--held->second == 0) heldCommands.erase(held);
            } else {
                xtlua_queue_log("XTLua: ignored unmatched command end: " + request.name + "\n");
            }
            break;
        }
        case CommandAction::once: XPLMCommandOnce(command); break;
        }
    }
    if(!pending.empty()) {
        std::lock_guard<std::mutex> lock(data_mutex);
        pending.insert(pending.end(), std::make_move_iterator(commandQueue.begin()),
                       std::make_move_iterator(commandQueue.end()));
        commandQueue.swap(pending);
    }
    // Overrides are created and consumed exclusively by the main thread.
    std::unordered_map<XPLMDataRef,float> newValues;
    for(XTControlObject* c:controlOverrides){
      try {
         
         if(c->srcDref==NULL || c->dstDref==NULL){ // Retry unresolved bindings.
            XTControlObject candidate=*c;
            printf("Do Create Override %s\n",candidate.data.c_str());
            json jData =json::parse(candidate.data);
            printf("Find src %s\n",jData["srcDref"].get<std::string>().c_str());
            candidate.srcDref=XPLMFindDataRef(jData["srcDref"].get<std::string>().c_str());
            printf("Find dst %s\n",jData["dstDref"].get<std::string>().c_str());
            candidate.dstDref=XPLMFindDataRef(jData["dstDref"].get<std::string>().c_str());
            candidate.dstIndex=-1;
            if(candidate.dstDref)
			{
				XPLMDataTypeID tid = XPLMGetDataRefTypes(candidate.dstDref);
                if(tid & (xplmType_FloatArray | xplmType_IntArray))			// AND are array type
				{
                   candidate.dstIndex =jData["dstIndex"].get<int>();
                }
            }
            if(jData.contains("scale"))
                candidate.scale=jData["scale"].get<float>();
            else
                candidate.scale=1.0;
            if(jData.contains("scaledref"))
            {
                 candidate.scaleDref=XPLMFindDataRef(jData["scaledref"].get<std::string>().c_str());
            } 
            if(jData.contains("min"))
            {
                candidate.min=jData["min"].get<float>();
            }
            if(jData.contains("max"))
            {
                candidate.max=jData["max"].get<float>();
            }
            candidate.minin=jData["minin"].get<float>();
            candidate.maxin=jData["maxin"].get<float>();
            candidate.minout=jData["minout"].get<float>();
            candidate.maxout=jData["maxout"].get<float>();
            *c=std::move(candidate);
         }
         
         if(c->srcDref!=NULL && c->dstDref!=NULL){
            float newVal=scale(c);
            if(newValues.find(c->dstDref)!=newValues.end()){
                float oldVal=newValues[c->dstDref];
                float minVal=c->minout;
                float maxVal=c->maxout;
                if(c->scale<0)
                {
                    float oldMin=minVal;
                    minVal=maxVal*-1;
                    maxVal=oldMin*-1;
                }
                if(oldVal<minVal)
                    minVal=oldVal;
                if(oldVal>maxVal)
                    maxVal=oldVal; 
                if(c->min>-9999)
                    minVal=c->min;
                if(c->max<9999)
                    maxVal=c->max;           
                newVal+=oldVal;
                if(newVal>maxVal)
                    newVal=maxVal;
                if(newVal<minVal)
                    newVal=minVal;    
                
                newValues[c->dstDref]=newVal;
 
            }
            else
                newValues[c->dstDref]=newVal;
         }
         else
            printf("Cant Override %s\n",c->data.c_str());
      } catch(const std::exception& error) {
          printf("XTLua: invalid control override: %s\n", error.what());
      }
    }
    for (auto x : newValues) {
        XPLMDataRef c=x.first;
        float val=newValues[c];
       /*if(c->dstIndex>=0){
            XPLMSetDatavf(c->dstDref, &val, c->dstIndex, 1);
        }
        else*/
        XPLMSetDataf(c, val);
    }
   // fireCmds.clear();
}
void XTLuaDataRefs::updateNavDataRefs(){
    if(mainDispatchDepth == 0) updateNavDataRefsImpl();
}
void XTLuaDataRefs::updateNavDataRefsImpl(){
    if(updatingNav || cleaning) return;
    MainDispatchScope dispatch(mainDispatchDepth, updatingNav);
    int entries=XPLMCountFMSEntries();
    int currentIndex=XPLMGetDestinationFMSEntry();
    int currentView=XPLMGetDisplayedFMSEntry();
    json dVdata = json::array();
    dVdata[0]=currentView+1;
    const std::string displayedEntry=dVdata.dump();
   // printf("currentView XPLMGetDisplayedFMSEntry=%d\n",currentView);
    json nVdata =json::array();
    float lastoutLat=0.0f;
    float lastoutLon=0.0f;
    int lastoutAltitude=0;
    bool hasLastOutput=false;
    int count=0;
    for(int i=0;i<entries;i++){
          XPLMNavType         outType=0;
          char                outID[256]={0};
          XPLMNavRef          outRef=XPLM_NAV_NOT_FOUND;
          int                 outAltitude=0;
          float               outLat=(std::numeric_limits<float>::quiet_NaN)();
          float               outLon=(std::numeric_limits<float>::quiet_NaN)();
          XPLMGetFMSEntryInfo(i,&outType,outID,&outRef,&outAltitude,&outLat,&outLon); 
          if(outRef!=XPLM_NAV_NOT_FOUND){
              XPLMNavType         outType2;   
              float               outLatitude;    
              float               outLongitude;   
              float               outHeight;    
              int                 outFrequency=0;   
              float               outHeading;    
                 
              char                outName[256]={0};    
              char                outReg[1]={0};
              XPLMGetNavAidInfo(outRef,&outType2,&outLatitude,&outLongitude,&outHeight,&outFrequency,&outHeading,outID,outName,outReg);
              //printf("getting XPLMGetNavAidInfo %d=%d, %d,%d ,%s\n",i,outRef,outType,outFrequency,outID); 
              double latDiff=outLatitude-outLat;
              if(latDiff>180)
                latDiff-=360;
              if(latDiff<-180)
                latDiff+=360;  
              double lonDiff=outLongitude-outLon;
              if(lonDiff>180)
                lonDiff-=360;
              if(lonDiff<-180)
                lonDiff+=360; 
              if(latDiff>-1 && latDiff < 1 && lonDiff>-1 && lonDiff < 1) { 
                nVdata[count]=json::array({outRef,outType,outFrequency,outHeading,outLatitude,outLongitude,string(outName),string(outID),outAltitude,(i==currentIndex)});
                lastoutLat=outLatitude;
                lastoutLon=outLongitude;
                lastoutAltitude=outAltitude;
                hasLastOutput=true;
              }
              else
              {
                 char val[256];
                sprintf(val,"%f %f",latDiff,lonDiff);
                 nVdata[count]=json::array({outRef,outType,0,0,outLat,outLon,string("latlon"),string("latlon"),outAltitude,(i==currentIndex)});
                 lastoutLat=outLat;
                lastoutLon=outLon;
                lastoutAltitude=outAltitude;
                hasLastOutput=true;
              }
              count++;
          }
          else{
                //printf("getting XPLMGetNavAidInfo %d=%d, %d %f,%f ,%s\n",i,outRef,outType,outLat,outLon,outID); 
              if (!std::isnan(outLat)&&!std::isnan(outLon)) {  
                nVdata[count]=json::array({outRef,outType,0,0,outLat,outLon,string("latlon"),string(outID),outAltitude,(i==currentIndex)});
                count++;
                lastoutLat=outLat;
                lastoutLon=outLon;
                lastoutAltitude=outAltitude;
                hasLastOutput=true;
              } else if(hasLastOutput){
                nVdata[count]=json::array({outRef,outType,0,0,lastoutLat,lastoutLon,string("latlon"),string(outID),lastoutAltitude,(i==currentIndex)});
                count++;
              }
          }
    }
    
    
    
    std::string fmsSnapshot=nVdata.dump();
    bool loadNavaids=false;
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        loadNavaids = navaids == nullptr;
    }
    NavAid* loadedHead=nullptr;
    NavAid* loadedTail=nullptr;
    if(loadNavaids){
        XPLMNavRef nAid=XPLMGetFirstNavAid();
        latR = XPLMFindDataRef("sim/flightmodel/position/latitude");
        lonR = XPLMFindDataRef("sim/flightmodel/position/longitude");
        while(nAid!=XPLM_NAV_NOT_FOUND){
                XPLMNavType         outType;    /* Can be NULL */
                float               outLatitude=-200;    /* Can be NULL */
                float               outLongitude=-200;    /* Can be NULL */
                float               outHeight;    /* Can be NULL */
                int                 outFrequency=0;    /* Can be NULL */
                float               outHeading=0;    /* Can be NULL */
                char                outID[32]={};    /* Can be NULL */
                char                outName[256]={};    /* Can be NULL */
                char                outReg[1]={};
                XPLMGetNavAidInfo(nAid,&outType,&outLatitude,&outLongitude,&outHeight,&outFrequency,&outHeading,outID,outName,outReg);
                /*double latDif=outLatitude-lat;
                double lonDif=outLongitude-lon;
                if(outType!=512&&latDif<2&&latDif>-2&&lonDif<2&&lonDif>-2)
                //if(outType!=512)
                    printf("%d=%d,%d,%f,%f,%f,%s\n",nAid,outType,outFrequency,latDif,lonDif,outHeading,outName); */
                if(outType!=512) {
                    NavAid* item=new NavAid{nAid, outType, outLatitude, outLongitude,
                                           outFrequency, outHeading, outName, outID, nullptr};
                    if(loadedTail) loadedTail->next=item;
                    else loadedHead=item;
                    loadedTail=item;
                }
                nAid=XPLMGetNextNavAid(nAid); 
            }
    }
    const double latitude=latR ? XPLMGetDatad(latR) : 0.0;
    const double longitude=lonR ? XPLMGetDatad(lonR) : 0.0;
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        incomingFMSString=std::move(fmsSnapshot);
        currentDisplayedEntry=displayedEntry;
        lat=latitude;
        lon=longitude;
        if(loadNavaids) {
            navaids=loadedHead;
            lastnavaid=loadedTail;
        }
    }
}
bool firstPass=true;
void XTLuaDataRefs::update_localNavData(){
    // Only this worker owns the incremental search. The main thread publishes
    // an immutable nav list and position snapshot; cleanup pauses the worker.
    double lat;
    double lon;
    NavAid* navSnapshot;
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        if(skipNaviads) return;
        lat=this->lat;
        lon=this->lon;
        navSnapshot=navaids;
    }
    if(current_navaid==NULL){
        current_navaid=navSnapshot;
         if(localNavaids.size()>0){
            std::vector<int> left;
            int count=0;
            json nVdata =json::array();
            for (auto x : localNavaids) {
                NavAid* val=x.second;
                double latDif=val->latitude-lat;
                
                double lonDif=val->longitude-lon;
                if(lonDif>180)
                    lonDif-=360;
                if(lonDif<-180)
                    lonDif+=360; 
                if(latDif>180)
                    latDif-=360;
                if(latDif<-180)
                    latDif+=360;     
                if(((latDif>2||latDif<-2||lonDif<-2||lonDif>2)&&val->type!=8)||(val->type==8&&(latDif>10||latDif<-10||lonDif<-10||lonDif>10)))
                    left.push_back(val->id);//localNavaids.erase(val->id);
                else{
                    nVdata[count]=json::array({val->id,val->type,val->frequency,val->heading,val->latitude,val->longitude,val->name,val->ident});
                    count++;
                }
                
            }
            

            {
                std::string snapshot=nVdata.dump();
                std::lock_guard<std::mutex> lock(data_mutex);
                incomingNavaidString=std::move(snapshot);
            }
            for (int id:left)
                localNavaids.erase(id);
            }
    }
    double latDif=lastUpdatelat-lat;
    double lonDif=lastUpdatelon-lon;
    if(lonDif>180)
        lonDif-=360;
    if(lonDif<-180)
        lonDif+=360; 
    if(latDif>180)
        latDif-=360;
    if(latDif<-180)
        latDif+=360; 
    if(latDif<0.5&&latDif>-0.5&&lonDif>-0.5&&lonDif<0.5)
        return;
    //printf("update_localNavData\n");    
    int count=0;
    //int cSize=localNavaids.size();
    while(current_navaid!=NULL&&(count<40||firstPass)){
        double latDif=current_navaid->latitude-lat;
        double lonDif=current_navaid->longitude-lon;
        if((current_navaid->type==8&&(latDif<10&&latDif>-10&&lonDif<10&&lonDif>-10))||(latDif<2&&latDif>-2&&lonDif<2&&lonDif>-2)){
            localNavaids[current_navaid->id]=current_navaid;
            //printf("%d=%d,%d,%f,%f,%f,%s\n",current_navaid->id,current_navaid->type,current_navaid->frequency,latDif,lonDif,current_navaid->heading,current_navaid->name.c_str());
        }
        current_navaid=current_navaid->next;
        count++;
    }
    if(current_navaid==NULL){
        if(localNavaids.size()>0){
            lastUpdatelat=lat;
            lastUpdatelon=lon;
            {
                std::lock_guard<std::mutex> lock(data_mutex);
                skipNaviads=true;
            }
            firstPass=false;
            //printf("completed pass\n");
        }
    }
    
    /*
   
    for (auto x : localNavaids) {
        NavAid* val=x.second;
        nVdata[count]=json::array({val->id,val->type,val->frequency,val->heading,val->latitude,val->longitude,val->name});
        count++;
    }

    
    localNavaidString=nVdata.dump();*/
}
struct PendingArrayRange
{
    int offset=0;
    std::vector<float> floats;
    std::vector<int> integers;
    std::vector<std::uint64_t> versions;
};

void XTLuaDataRefs::updateArrayDataRef(const std::string& name)
{
    ArrayBridgeRef info;
    std::vector<std::uint64_t> baselineVersions;
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        auto found = arrayDataRefs.find(name);
        if(found == arrayDataRefs.end())
            return;
        info = found->second;
        const std::vector<XTLuaArrayFloat*>& cached = floatdataRefs[name];
        baselineVersions.reserve(cached.size());
        for(const XTLuaArrayFloat* item : cached)
            baselineVersions.push_back(item->version);
    }

    // All SDK access stays on the main thread, outside the worker-cache lock.
    int sdkSize = info.type == xplmType_IntArray
        ? XPLMGetDatavi(info.ref, NULL, 0, 0)
        : XPLMGetDatavf(info.ref, NULL, 0, 0);
    if(sdkSize < 0)
        sdkSize = 0;
    std::vector<double> snapshot(static_cast<size_t>(sdkSize));
    if(sdkSize != 0)
    {
        int copied = 0;
        if(info.type == xplmType_IntArray)
        {
            std::vector<int> sdkValues(static_cast<size_t>(sdkSize));
            copied = XPLMGetDatavi(info.ref, sdkValues.data(), 0, sdkSize);
            for(int i = 0; i < copied && i < sdkSize; ++i)
                snapshot[static_cast<size_t>(i)] = sdkValues[static_cast<size_t>(i)];
        }
        else
        {
            std::vector<float> sdkValues(static_cast<size_t>(sdkSize));
            copied = XPLMGetDatavf(info.ref, sdkValues.data(), 0, sdkSize);
            for(int i = 0; i < copied && i < sdkSize; ++i)
                snapshot[static_cast<size_t>(i)] = sdkValues[static_cast<size_t>(i)];
        }
        if(copied < sdkSize)
            snapshot.resize(static_cast<size_t>((std::max)(0, copied)));
    }

    std::vector<PendingArrayRange> pending;
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        auto found = arrayDataRefs.find(name);
        if(found == arrayDataRefs.end() || found->second.ref != info.ref)
            return;
        std::vector<XTLuaArrayFloat*>& cached = floatdataRefs[name];
        const size_t desired = snapshot.size();
        const size_t previous = cached.size();
        if(desired < previous)
        {
            std::unordered_map<int, double>& deferred = deferredArrayWrites[name];
            for(size_t i = desired; i < previous; ++i)
            {
                if(cached[i]->set)
                    deferred[static_cast<int>(i)] = cached[i]->value;
                delete cached[i];
            }
            cached.resize(desired);
        }
        else if(desired > previous)
        {
            cached.reserve(desired);
            std::unordered_map<int, double>& deferred = deferredArrayWrites[name];
            for(size_t i = previous; i < desired; ++i)
            {
                XTLuaArrayFloat* item = new XTLuaArrayFloat;
                item->ref = info.ref;
                item->type = info.type;
                item->index = static_cast<int>(i);
                item->value = snapshot[i];
                auto saved = deferred.find(static_cast<int>(i));
                if(saved != deferred.end())
                {
                    item->value = saved->second;
                    item->set = true;
                    ++item->version;
                    deferred.erase(saved);
                }
                cached.push_back(item);
            }
        }
        for(size_t i = 0; i < desired; ++i)
        {
            XTLuaArrayFloat* item = cached[i];
            if(!item->set && (i >= baselineVersions.size() || item->version == baselineVersions[i]))
                item->value = snapshot[i];
            item->get = false;
        }
        for(size_t i = 0; i < desired; )
        {
            if(!cached[i]->set) { ++i; continue; }
            PendingArrayRange range;
            range.offset = static_cast<int>(i);
            do
            {
                const XTLuaArrayFloat* item = cached[i];
                if(info.type == xplmType_IntArray)
                    range.integers.push_back(xtlua_array_to_int(item->value));
                else
                    range.floats.push_back(static_cast<float>(item->value));
                range.versions.push_back(item->version);
                ++i;
            } while(i < desired && cached[i]->set);
            pending.push_back(std::move(range));
        }
    }

    for(PendingArrayRange& range : pending)
    {
        if(info.type == xplmType_IntArray)
            XPLMSetDatavi(info.ref, range.integers.data(),
                           range.offset, static_cast<int>(range.integers.size()));
        else
            XPLMSetDatavf(info.ref, range.floats.data(),
                           range.offset, static_cast<int>(range.floats.size()));
    }
    if(pending.empty())
        return;
    const int afterWriteSize = info.type == xplmType_IntArray
        ? XPLMGetDatavi(info.ref, NULL, 0, 0)
        : XPLMGetDatavf(info.ref, NULL, 0, 0);
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        auto found = arrayDataRefs.find(name);
        if(found == arrayDataRefs.end() || found->second.ref != info.ref)
            return;
        const std::vector<XTLuaArrayFloat*>& cached = floatdataRefs[name];
        for(const PendingArrayRange& range : pending)
            for(size_t j = 0; j < range.versions.size(); ++j)
            {
                const size_t index = static_cast<size_t>(range.offset) + j;
                if(index < cached.size() && index < static_cast<size_t>((std::max)(0, afterWriteSize)) &&
                   cached[index]->version == range.versions[j])
                    cached[index]->set = false;
            }
    }
}

void XTLuaDataRefs::updateFloatDataRefs(){
    if(mainDispatchDepth == 0) updateFloatDataRefsImpl();
}
void XTLuaDataRefs::updateFloatDataRefsImpl(){
    if(updatingFloats || cleaning) return;
    MainDispatchScope dispatch(mainDispatchDepth, updatingFloats);
    struct Transfer {
        std::string name;
        XPLMDataRef ref;
        int type;
        std::uint64_t version;
        bool write;
        double value;
    };
    std::vector<Transfer> batch;
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        const bool allGet = ((simTime - beginFlightTime) < 10);
        for(const auto& entry : floatdataRefs) {
            if(arrayDataRefs.find(entry.first) != arrayDataRefs.end() || entry.second.empty())
                continue;
            XTLuaArrayFloat* val=entry.second[0];
            if(!allGet && !val->get && !val->set) continue;
            batch.push_back({entry.first, val->ref, val->type, val->version, val->set, val->value});
            val->get=false;
        }
        changeddataRefs.clear();
    }
    for(const Transfer& item : batch) {
        if(item.write) {
            if(item.type == xplmType_Double) XPLMSetDatad(item.ref, item.value);
            else if(item.type == xplmType_Int) XPLMSetDatai(item.ref, xtlua_array_to_int(item.value));
            else XPLMSetDataf(item.ref, static_cast<float>(item.value));
        }
        const double snapshot=read_scalar_snapshot(item.ref, item.type);
        std::lock_guard<std::mutex> lock(data_mutex);
        auto found=floatdataRefs.find(item.name);
        if(found == floatdataRefs.end() || found->second.empty()) continue;
        XTLuaArrayFloat* val=found->second[0];
        if(val->ref != item.ref || val->type != item.type || val->version != item.version)
            continue; // A newer worker write owns this cache entry now.
        if(item.write) val->set=false;
        if(!val->set) val->value=snapshot;
    }
}

double XTLuaDataRefs::XTGetElapsedTime(){
    return timeT.load(std::memory_order_acquire);
}

void XTLuaDataRefs::refreshAllDataRefs(){
    if(mainDispatchDepth != 0 || refreshingDataRefs || cleaning) return;
    MainDispatchScope dispatch(mainDispatchDepth, refreshingDataRefs);
    std::vector<std::string> arrayNames;
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        for(const auto& entry : stringdataRefs)
            if(entry.second) entry.second->get=true;
        for(const auto& entry : floatdataRefs) {
            if(arrayDataRefs.find(entry.first) != arrayDataRefs.end()) {
                arrayNames.push_back(entry.first);
            } else if(!entry.second.empty()) {
                entry.second[0]->get=true;
            }
        }
    }
    // Use the same versioned transfers for refresh and the regular flightloop.
    updateStringDataRefsImpl();
    updateFloatDataRefsImpl();
    for(const std::string& name : arrayNames)
        updateArrayDataRef(name);
}

void XTLuaDataRefs::updateDataRefs(){
    if(mainDispatchDepth != 0 || updatingDataRefs || cleaning) return;
    MainDispatchScope dispatch(mainDispatchDepth, updatingDataRefs);
    isPaused=paused_ref ? XPLMGetDatai(paused_ref) : 1;
    simTime=sim_time_ref ? XPLMGetDataf(sim_time_ref) : 0.0;
    timeT.store(simTime, std::memory_order_release);
    const bool replay=replay_ref && XPLMGetDatai(replay_ref) != 0;

    // Side effects enqueued by the worker are consumed from an owned batch.
    updateMainThreadRequests();
    if(updateRoll == 0 && !replay)
        updateNavDataRefsImpl();
    if(updateRoll == 5) {
        updateStringDataRefsImpl();
        updateRoll=-1;
    }
    updateFloatDataRefsImpl();
    updateCommandsImpl();
    ++updateRoll;
    serialWindow.show();
    std::vector<std::string> activeArrayNames;
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        for(const auto& array : arrayDataRefs)
            if(array.second.active) activeArrayNames.push_back(array.first);
    }
    for(const std::string& name : activeArrayNames)
        updateArrayDataRef(name);
}

void XTLuaDataRefs::cleanup(){
    assert(mainDispatchDepth == 0);
    if(mainDispatchDepth != 0 || cleaning) return;
    MainDispatchScope dispatch(mainDispatchDepth, cleaning);
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        acceptingRequests=false;
        commandQueue.clear();
        mainThreadQueue.clear();
    }
    // SDK End callbacks can reenter the plugin. No worker/cache lock is held;
    // lifecycle work is deferred while this dispatch guard is active.
    auto held=std::move(heldCommands);
    heldCommands.clear();
    unresolvedCommands.clear();
    for(const auto& entry : held)
        for(unsigned i=0; i<entry.second; ++i) XPLMCommandEnd(entry.first);
    // Camera state is main-owned and released before caches are destroyed.
    extern bool controllingCam;
    extern int wantsCamera;
    wantsCamera=0;
    if(controllingCam) XPLMDontControlCamera();
    controllingCam=false;
    std::lock_guard<std::mutex> lock(data_mutex);
    drefResolveQueue.clear();
    cmdResolveQueue.clear();
    cmdHandlerResolveQueue.clear();
    for(const auto& entry : floatdataRefs)
        for(XTLuaArrayFloat* item : entry.second) delete item;
    floatdataRefs.clear();
    arrayDataRefs.clear();
    deferredArrayWrites.clear();
    for(const auto& entry : stringdataRefs) delete entry.second;
    stringdataRefs.clear();
    while(navaids) {
        NavAid* next=navaids->next;
        delete navaids;
        navaids=next;
    }
    for(XTControlObject* item : controlOverrides) delete item;
    controlOverrides.clear();
    lastnavaid=nullptr;
    current_navaid=nullptr;
    latR=nullptr;
    lonR=nullptr;
    lat=lon=lastUpdatelat=lastUpdatelon=0.0;
    skipNaviads=true;
    firstPass=true;
    changeddataRefs.clear();
    localNavaids.clear();
    incomingNavaidString.clear();
    localNavaidString.clear();
    incomingFMSString.clear();
    localFMSString.clear();
    currentDisplayedEntry.clear();
    updateRoll=0;
    acceptingRequests=true;
}

void XTLuaDataRefs::XTqueueresolve_dref(xtlua_dref * d){
    data_mutex.lock();
    //printf("will resolve %s\n",d->m_name.c_str());
    drefResolveQueue.push_back(d);//this needs to be done on another thread
    data_mutex.unlock();
}
void XTLuaDataRefs::XTqueueresolve_cmd(xtlua_cmd * d){
    data_mutex.lock();
    cmdResolveQueue.push_back(d);//this needs to be done on another thread
    data_mutex.unlock();
}
 



int XTLuaDataRefs::resolveQueue(){
    if(mainDispatchDepth != 0 || resolving || cleaning) return 0;
    MainDispatchScope dispatch(mainDispatchDepth, resolving);
    if(!paused_ref) paused_ref=XPLMFindDataRef("sim/time/paused");
    if(!replay_ref) replay_ref=XPLMFindDataRef("sim/time/is_in_replay");
    if(!sim_time_ref) {
        sim_time_ref=XPLMFindDataRef("sim/time/total_running_time_sec");
        if(sim_time_ref) beginFlightTime=XPLMGetDataf(sim_time_ref);
    }
    std::vector<xtlua_dref*> datarefs;
    std::vector<xtlua_cmd*> commands;
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        datarefs.swap(drefResolveQueue);
        commands.swap(cmdResolveQueue);
    }
    int resolvedCount=0;
    std::vector<xtlua_dref*> pendingDatarefs;
    std::vector<xtlua_cmd*> pendingCommands;
    for(xtlua_dref* d : datarefs) {
        if(!d || d->m_resolved.load(std::memory_order_acquire)) continue;
        if(d->m_name.rfind("xtlua/", 0) == 0) {
            d->m_types=xplmType_Data;
            d->m_resolved.store(true, std::memory_order_release);
            ++resolvedCount;
            continue;
        }
        // Metadata is private to the resolver until m_resolved is published.
        d->m_ours=0;
        d->local_dref=nullptr;
        d->m_index=-1;
        d->m_types=0;
        grabLocal(d);
        XPLMDataRef ref=XPLMFindDataRef(d->m_name.c_str());
        int index=-1;
        int types=ref ? XPLMGetDataRefTypes(ref) : 0;
        if(!ref) {
            const auto open=d->m_name.find('[');
            const auto close=d->m_name.find(']');
            if(open != std::string::npos && open > 0 && close != std::string::npos &&
               close > open+1 && close == d->m_name.size()-1) {
                const std::string number=d->m_name.substr(open+1, close-open-1);
                char* end=nullptr;
                errno=0;
                const long candidate=std::strtol(number.c_str(), &end, 10);
                if(errno != ERANGE && end != number.c_str() && *end == '\0' && candidate >= 0 &&
                   candidate <= (std::numeric_limits<int>::max)()) {
                    ref=XPLMFindDataRef(d->m_name.substr(0, open).c_str());
                    types=ref ? XPLMGetDataRefTypes(ref) : 0;
                    if(types & (xplmType_FloatArray | xplmType_IntArray))
                        index=static_cast<int>(candidate);
                    else ref=nullptr;
                }
            }
        }
        if(!ref || !types) {
            pendingDatarefs.push_back(d);
            continue;
        }
        d->m_dref=ref;
        d->m_types=types;
        d->m_index=index;
        const std::string name=bridge_key(ref);
        bool exists=false;
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            exists=floatdataRefs.find(name) != floatdataRefs.end() ||
                   stringdataRefs.find(name) != stringdataRefs.end();
        }
        if(!exists) {
            std::vector<XTLuaArrayFloat*> initialNumbers;
            XTLuaCharArray* initialString=nullptr;
            int arrayType=0;
            if(types & (xplmType_FloatArray | xplmType_IntArray)) {
                arrayType=(types & xplmType_FloatArray) ? xplmType_FloatArray : xplmType_IntArray;
                int size=arrayType == xplmType_FloatArray
                    ? XPLMGetDatavf(ref, nullptr, 0, 0) : XPLMGetDatavi(ref, nullptr, 0, 0);
                size=(std::max)(0, size);
                std::vector<double> snapshot(static_cast<size_t>(size));
                if(size > 0) {
                    int copied=0;
                    if(arrayType == xplmType_FloatArray) {
                        std::vector<float> values(static_cast<size_t>(size));
                        copied=XPLMGetDatavf(ref, values.data(), 0, size);
                        size=(std::max)(0, (std::min)(size, copied));
                        for(int i=0; i<size; ++i) snapshot[i]=values[i];
                    } else {
                        std::vector<int> values(static_cast<size_t>(size));
                        copied=XPLMGetDatavi(ref, values.data(), 0, size);
                        size=(std::max)(0, (std::min)(size, copied));
                        for(int i=0; i<size; ++i) snapshot[i]=values[i];
                    }
                }
                for(int i=0; i<size; ++i) {
                    XTLuaArrayFloat* item=new XTLuaArrayFloat;
                    item->ref=ref;
                    item->type=arrayType;
                    item->index=i;
                    item->value=snapshot[i];
                    item->get=true;
                    initialNumbers.push_back(item);
                }
            } else if(types & (xplmType_Double | xplmType_Float | xplmType_Int)) {
                XTLuaArrayFloat* item=new XTLuaArrayFloat;
                item->ref=ref;
                item->type=(types & xplmType_Double) ? xplmType_Double :
                           (types & xplmType_Float) ? xplmType_Float : xplmType_Int;
                item->value=read_scalar_snapshot(ref, item->type);
                item->get=true;
                initialNumbers.push_back(item);
            }
            // Providers may legally expose Data alongside numeric types.
            // Publish both caches so the public Data-first Lua type remains valid.
            if(types & xplmType_Data) {
                initialString=new XTLuaCharArray;
                initialString->ref=ref;
                initialString->value=read_string_snapshot(ref);
                initialString->get=true;
            }
            {
                std::lock_guard<std::mutex> lock(data_mutex);
                if(initialString) stringdataRefs.emplace(name, initialString);
                if(arrayType || !initialNumbers.empty()) {
                    floatdataRefs.emplace(name, std::move(initialNumbers));
                    if(arrayType) arrayDataRefs[name]={ref, arrayType, false};
                }
            }
        }
        d->m_resolved.store(true, std::memory_order_release);
        ++resolvedCount;
    }
    for(xtlua_cmd* command : commands) {
        if(!command) continue;
        const XPLMCommandRef ref=XPLMFindCommand(command->m_name.c_str());
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            command->m_cmd=ref;
        }
        if(ref) ++resolvedCount;
        else pendingCommands.push_back(command);
    }
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        drefResolveQueue.insert(drefResolveQueue.begin(), pendingDatarefs.begin(), pendingDatarefs.end());
        cmdResolveQueue.insert(cmdResolveQueue.begin(), pendingCommands.begin(), pendingCommands.end());
    }
    return resolvedCount;
}
std::vector<xtlua_cmd*> XTLuaDataRefs::XTGetHandlers(){
    std::vector<xtlua_cmd*> retval;
    if(mainDispatchDepth != 0) return retval;
    std::lock_guard<std::mutex> lock(data_mutex);
    for (auto it=cmdHandlerResolveQueue.begin(); it!=cmdHandlerResolveQueue.end();) {
        xtlua_cmd * cmd=it->second;
        if(cmd != nullptr && cmd->m_cmd != nullptr){
            retval.push_back(cmd);
            it=cmdHandlerResolveQueue.erase(it);
        }
        else
            ++it;
    }
    return retval;
}
double XTLuaDataRefs::XTGetDataf(xtlua_dref * d, bool local){
    if(!d || !d->m_resolved.load(std::memory_order_acquire)) return 0.0;
    if(d->m_ours) return xlua_dref_get_number(d->local_dref);
    std::lock_guard<std::mutex> lock(data_mutex);
    auto found=floatdataRefs.find(bridge_key(d->m_dref));
    if(found == floatdataRefs.end() || found->second.empty()) return 0.0;
    XTLuaArrayFloat* val=found->second[0];
    val->get=true;
    return val->value;
}

void XTLuaDataRefs::XTSetDataf(xtlua_dref * d, double value, bool local){
    if(!d || !d->m_resolved.load(std::memory_order_acquire)) return;
    if(d->m_ours) {
        xlua_dref_set_number(d->local_dref, value);
        return;
    }
    std::lock_guard<std::mutex> lock(data_mutex);
    auto found=floatdataRefs.find(bridge_key(d->m_dref));
    if(found == floatdataRefs.end() || found->second.empty()) return;
    XTLuaArrayFloat* val=found->second[0];
    if(val->type == xplmType_Int) value=xtlua_array_to_int(value);
    else if(val->type == xplmType_Float) value=static_cast<float>(value);
    // Even a repeated same-value write is a new generation: an SDK callback
    // may be in flight with an earlier value and must not acknowledge this one.
    val->value=value;
    val->set=true;
    ++val->version;
}

std::string XTLuaDataRefs::XTGetString(xtlua_dref * d){
    if(!d) return {};
    const bool synthetic=d->m_name.rfind("xtlua/", 0) == 0;
    if(!synthetic && !d->m_resolved.load(std::memory_order_acquire)) return {};
    if(!synthetic && d->m_ours) return xlua_dref_get_string(d->local_dref);
    std::lock_guard<std::mutex> lock(data_mutex);
    if(d->m_name.rfind("xtlua/xpFMSData", 0) == 0)
        return currentDisplayedEntry;
    if(d->m_name.rfind("xtlua/navaids", 0) == 0) {
        skipNaviads=false;
        return incomingNavaidString;
    }
    if(d->m_name.rfind("xtlua/fms", 0) == 0) {
        skipNaviads=false;
        return incomingFMSString;
    }
    if(synthetic) return {};
    auto found=stringdataRefs.find(bridge_key(d->m_dref));
    if(found == stringdataRefs.end()) return {};
    found->second->get=true;
    return found->second->value;
}

int XTLuaDataRefs::XTGetDatab(xtlua_dref * d, void * outValue,
                            int offset, int maxBytes, bool local){
    if(offset < 0 || maxBytes < 0) return 0;
    const std::string snapshot=XTGetString(d);
    if(!outValue)
        return static_cast<int>((std::min)(snapshot.size(),
                            static_cast<size_t>((std::numeric_limits<int>::max)())));
    const size_t start=static_cast<size_t>(offset);
    if(start >= snapshot.size()) return 0;
    const size_t copied=(std::min)(snapshot.size()-start, static_cast<size_t>(maxBytes));
    std::memcpy(outValue, snapshot.data()+start, copied);
    return static_cast<int>(copied);
}
float camData[5]={0};
bool controllingCam=false;
int wantsCamera=0;
int XTLuaCameraFunc(
                                   XPLMCameraPosition_t * outCameraPosition,   
                                   int                  inIsLosingControl,    
                                   void *               inRefcon)
{
	if (outCameraPosition && !inIsLosingControl)
	{

		outCameraPosition->x = camData[0];
		outCameraPosition->y = camData[1];
		outCameraPosition->z = camData[2];
		outCameraPosition->pitch = camData[3];
		outCameraPosition->heading = camData[4];
		outCameraPosition->roll = 0;		
        outCameraPosition->zoom = 1.0f;
        //printf("XTLua Did Cam Control %f %f %f %f %f\n",camData[0],camData[1],camData[2],camData[3],camData[4]);
	}
    else{
        controllingCam=false;
    }
    //printf("XTLua Cam Control %f %f %f %f %f\n",camData[0],camData[1],camData[2],camData[3],camData[4]);
    /* Return 0 to indicate we do not want to keep controlling the camera. */
    int retVal=wantsCamera;
    if(retVal==0){
        controllingCam=false;
    }
	return retVal;
}
void XTLuaDataRefs::XTSetDatab(xtlua_dref * d, std::string value){
    if(!d) return;
    if(d->m_name.rfind("xtlua/", 0) == 0) {
        std::lock_guard<std::mutex> lock(data_mutex);
        if(acceptingRequests)
            mainThreadQueue.push_back({d->m_name, std::move(value)});
        return;
    }
    if(!d->m_resolved.load(std::memory_order_acquire)) return;
    if(d->m_ours) {
        xlua_dref_set_string(d->local_dref, value);
        return;
    }
    if(value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return;
    std::lock_guard<std::mutex> lock(data_mutex);
    auto found=stringdataRefs.find(bridge_key(d->m_dref));
    if(found == stringdataRefs.end()) return;
    XTLuaCharArray* val=found->second;
    val->value=std::move(value);
    val->set=true;
    ++val->version;
}

void XTLuaDataRefs::updateMainThreadRequests(){
    if(updatingMainRequests || cleaning) return;
    MainDispatchScope dispatch(mainDispatchDepth, updatingMainRequests);
    std::vector<MainThreadRequest> batch;
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        batch.swap(mainThreadQueue);
    }
    for(const MainThreadRequest& request : batch) {
        try {
            applyMainThreadRequest(request);
        } catch(const std::exception& error) {
            printf("XTLua: invalid main-thread request %s: %s\n",
                   request.name.c_str(), error.what());
        }
    }
}

void XTLuaDataRefs::applyMainThreadRequest(const MainThreadRequest& request){
    const std::string& name=request.name;
    const std::string& value=request.value;
    if(name.rfind("xtlua/getserial", 0) == 0) {
        serialWindow.init(value);
    } else if(name.rfind("xtlua/currentFMSID", 0) == 0) {
        size_t consumed=0;
        const long index=std::stol(value, &consumed);
        if(consumed != value.size() || index <= 0 || index > XPLMCountFMSEntries())
            return;
        XPLMSetDestinationFMSEntry(static_cast<int>(index-1));
    } else if(name.rfind("xtlua/loadtoFMS", 0) == 0) {
        const auto load=reinterpret_cast<XPLMLoadFMSFlightPlan_f>(XPLMFindSymbol("XPLMLoadFMSFlightPlan"));
        if(!load || value.size() > (std::numeric_limits<unsigned int>::max)()) return;
        const int count=XPLMCountFMSEntries();
        for(int i=count-1; i>=0; --i) XPLMClearFMSEntry(i);
        load(0, value.c_str(), static_cast<unsigned int>(value.size()));
    } else if(name.rfind("xtlua/currentFMS", 0) == 0) {
        const int count=XPLMCountFMSEntries();
        for(int i=0; i<count; ++i) {
            char ident[256]={};
            XPLMGetFMSEntryInfo(i, nullptr, ident, nullptr, nullptr, nullptr, nullptr);
            if(value == ident) {
                XPLMSetDestinationFMSEntry(i);
                break;
            }
        }
    } else if(name.rfind("xtlua/controlObject", 0) == 0) {
        // Parse/validate before adding an object that lives only on the main thread.
        const json data=json::parse(value);
        data.at("srcDref").get<std::string>();
        data.at("dstDref").get<std::string>();
        for(const char* field : {"minin", "maxin", "minout", "maxout"})
            if(!std::isfinite(data.at(field).get<double>())) return;
        if(data.at("maxin").get<double>() == data.at("minin").get<double>()) return;
        XTControlObject* control=new XTControlObject;
        control->data=value;
        controlOverrides.push_back(control);
    } else if(name.rfind("xtlua/camera", 0) == 0) {
        const std::vector<double> values=json::parse(value).get<std::vector<double>>();
        if(values.size() < 5) return;
        bool enabled=false;
        for(size_t i=0; i<5; ++i) {
            if(!std::isfinite(values[i]) ||
               std::abs(values[i]) > (std::numeric_limits<float>::max)()) return;
            enabled=enabled || values[i] != 0.0;
        }
        for(size_t i=0; i<5; ++i) camData[i]=static_cast<float>(values[i]);
        wantsCamera=enabled ? 1 : 0;
        if(!enabled && controllingCam) {
            XPLMDontControlCamera();
            controllingCam=false;
        } else if(enabled && !controllingCam) {
            controllingCam=true;
            XPLMControlCamera(xplm_ControlCameraUntilViewChanges, XTLuaCameraFunc, nullptr);
        }
    } else if(name.rfind("xtlua/fltpln", 0) == 0) {
        // Validate the complete payload before modifying the current FMS plan.
        const std::vector<std::vector<double>> waypoints=
            json::parse(value).get<std::vector<std::vector<double>>>();
        if(waypoints.size() > 100) return; // XPLM FMS entry limit.
        for(const auto& waypoint : waypoints) {
            if(waypoint.size() < 3 || !std::isfinite(waypoint[0]) ||
               !std::isfinite(waypoint[1]) || !std::isfinite(waypoint[2]) ||
               waypoint[0] < -90.0 || waypoint[0] > 90.0 ||
               waypoint[1] < -180.0 || waypoint[1] > 180.0) return;
        }
        const int count=XPLMCountFMSEntries();
        for(int i=count-1; i>=0; --i) XPLMClearFMSEntry(i);
        for(size_t i=0; i<waypoints.size(); ++i)
            XPLMSetFMSEntryLatLon(static_cast<int>(i), static_cast<float>(waypoints[i][0]),
                                 static_cast<float>(waypoints[i][1]),
                                 xtlua_array_to_int(waypoints[i][2]));
    }
}
int XTLuaDataRefs::XTGetDatavf(
                                   xtlua_dref * d,    
                                   double *             outValues,    /* Can be NULL */
                                   int                  inOffset,    
                                   int                  inMax,bool local)
{
    if(!d || !d->m_resolved.load(std::memory_order_acquire) || inOffset < 0 || inMax < 0)
        return 0;
    int retVal=0;
    data_mutex.lock();
    //outValue will be set to an array of floats
    char namec[32];
    XPLMDataRef  inDataRef=d->m_dref;
    sprintf(namec,"%p",inDataRef);
    std::string name=namec;
    auto activeArray = arrayDataRefs.find(name);
    if(activeArray != arrayDataRefs.end())
        activeArray->second.active = true;
    
    
     
    
    {
        if(outValues!=NULL){
            //if(!d->m_ours)
            if(floatdataRefs.find(name)!=floatdataRefs.end()){
                const std::vector<XTLuaArrayFloat*>& val=floatdataRefs[name];
                for(unsigned int i=inOffset;i<val.size()&&i-inOffset<(unsigned int)inMax;i++){
                     if(!d->m_ours){
                        outValues[i-inOffset]=val[i]->value;
                        val[i]->get=true;
                     }
                     else{
                         outValues[i-inOffset]= xlua_dref_get_array(d->local_dref,i);
                         val[i]->value=outValues[i-inOffset];
                     }
                    retVal++;
                    //printf("apply XTGetDatavf %s %s[%d/%d] %s = %f\n",d->m_name.c_str(),name.c_str(),inOffset,inMax,outValues!=NULL?"values":"size",val[i]->value);
                }
            }
        }
        else
        {
            if(floatdataRefs.find(name)!=floatdataRefs.end()){
                const std::vector<XTLuaArrayFloat*>& val=floatdataRefs[name];
                retVal=(int)val.size();
                //data_mutex.unlock();
                //return retVal;
            }
 
        }
        
    }
    
    data_mutex.unlock();
    return retVal;

}

void XTLuaDataRefs::XTSetDatavf(
                                   xtlua_dref * d,    
                                   double               inValue,
                                   int                  index)
{
    if(!d || !d->m_resolved.load(std::memory_order_acquire) || index < 0)
        return;
    data_mutex.lock();
    char namec[32];
    XPLMDataRef  inDataRef=d->m_dref;
    sprintf(namec,"%p",inDataRef);
    std::string name=namec;
    auto activeArray = arrayDataRefs.find(name);
    if(activeArray != arrayDataRefs.end())
        activeArray->second.active = true;
  
    if(floatdataRefs.find(name)!=floatdataRefs.end()){
        std::vector<XTLuaArrayFloat*>& val=floatdataRefs[name];
        if(activeArray != arrayDataRefs.end() && activeArray->second.type == xplmType_IntArray)
            inValue = xtlua_array_to_int(inValue);
        if(index<(int)val.size()){
            if(!d->m_ours){
                val[index]->set=true;
                ++val[index]->version;
            }
            else{
                xlua_dref_set_array(d->local_dref,index,inValue);
                if(val[index]->value != inValue)
                    ++val[index]->version;
            }
            val[index]->value=inValue;
            
          //printf("apply XTSetDatavf single %s %s[%d] = %f\n",d->m_name.c_str(),name.c_str(),index,val[index]->value); 
        }
        else if(!d->m_ours){
            //printf("apply XTSetDatavf overflow %s %s[%d] = %f\n",d->m_name.c_str(),name.c_str(),index,inValue); 
            int n=(int)val.size();
            for(;n<index;n++){
                XTLuaArrayFloat* v=new XTLuaArrayFloat;
                v->ref=d->m_dref;
                v->type=(d->m_types & xplmType_FloatArray) ? xplmType_FloatArray : xplmType_IntArray;
                v->value=0.0;
                v->get=true;
                v->index=n;
                floatdataRefs[name].push_back(v);
            }
            XTLuaArrayFloat* v=new XTLuaArrayFloat;
            v->ref=d->m_dref;
            v->type=(d->m_types & xplmType_FloatArray) ? xplmType_FloatArray : xplmType_IntArray;
            v->value=inValue;
            v->set=true;
            v->version=1;
            //printf("did apply XTSetDatavf overflow %s %s[%d] = %f\n",d->m_name.c_str(),name.c_str(),n,inValue); 
            v->index=n;
            floatdataRefs[name].push_back(v);
        }
    }
    data_mutex.unlock();
}    

std::vector<double> XTLuaDataRefs::XTGetArrayValues(xtlua_dref * d, int offset, int count)
{
    if(!d || !d->m_resolved.load(std::memory_order_acquire) || offset < 0 || count < 0)
        return {};
    std::lock_guard<std::mutex> lock(data_mutex);
    char namec[32] = {0};
    sprintf(namec, "%p", d->m_dref);
    const std::string name(namec);
    auto activeArray = arrayDataRefs.find(name);
    if(activeArray != arrayDataRefs.end())
        activeArray->second.active = true;
    auto found = floatdataRefs.find(name);
    if(found == floatdataRefs.end())
        return {};
    const std::vector<XTLuaArrayFloat*>& cached = found->second;
    const size_t start = static_cast<size_t>(offset);
    const size_t length = static_cast<size_t>(count);
    if(start > cached.size() || length > cached.size() - start)
        return {};

    std::vector<double> result;
    result.reserve(length);
    if(d->m_ours)
    {
        if(!d->local_dref)
            return {};
        result = xlua_dref_get_array_values(d->local_dref, offset, count);
        if(result.size() != length)
            return {};
        for(size_t i = 0; i < length; ++i)
            cached[start + i]->value = result[i];
    }
    else
    {
        for(size_t i = 0; i < length; ++i)
        {
            XTLuaArrayFloat* item = cached[start + i];
            result.push_back(item->value);
            item->get = true;
        }
    }
    return result;
}

int XTLuaDataRefs::XTSetArrayValues(xtlua_dref * d, const std::vector<double>& values, int offset)
{
    if(!d || !d->m_resolved.load(std::memory_order_acquire) || offset < 0)
        return -1;
    std::lock_guard<std::mutex> lock(data_mutex);
    char namec[32] = {0};
    sprintf(namec, "%p", d->m_dref);
    const std::string name(namec);
    auto activeArray = arrayDataRefs.find(name);
    if(activeArray != arrayDataRefs.end())
        activeArray->second.active = true;
    auto found = floatdataRefs.find(name);
    if(found == floatdataRefs.end())
        return -1;
    std::vector<XTLuaArrayFloat*>& cached = found->second;
    const size_t start = static_cast<size_t>(offset);
    if(start > cached.size() || values.size() > cached.size() - start)
        return -1;

    std::vector<double> normalized;
    normalized.reserve(values.size());
    const bool integerArray = activeArray != arrayDataRefs.end() &&
        activeArray->second.type == xplmType_IntArray;
    for(double value : values)
        normalized.push_back(integerArray ? xtlua_array_to_int(value) : value);
    if(d->m_ours)
    {
        if(!d->local_dref || xlua_dref_set_array_values(d->local_dref, normalized, offset) < 0)
            return -1;
    }
    for(size_t i = 0; i < normalized.size(); ++i)
    {
        XTLuaArrayFloat* item = cached[start + i];
        if(!d->m_ours)
        {
            ++item->version;
            item->set = true;
        }
        else if(item->value != normalized[i])
            ++item->version;
        item->value = normalized[i];
    }
    return static_cast<int>(values.size());
}


                        
