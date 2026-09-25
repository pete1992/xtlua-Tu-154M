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
#include "XPLMCamera.h"
#include <stdio.h>
#include <assert.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include "SerialWidget.h"
#include "json/json.hpp"
#include <vector>
using nlohmann::json;
static std::mutex data_mutex;
typedef void (*XPLMLoadFMSFlightPlan_f)(int inDevice, const char * inBuffer, unsigned int inBufferLen);

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
    data_mutex.lock();
    printf("Command start %s\n",cmd->m_name.c_str());
    XTCmd* xtcmd=new XTCmd();
    xtcmd->xluaref=cmd;
    xtcmd->start=true;
    xtcmd->stop=false;
    xtcmd->fire=false;
    commandQueue.push_back(xtcmd);
    data_mutex.unlock();
}

void XTLuaDataRefs::XTCommandEnd(xtlua_cmd * cmd){
    data_mutex.lock();

    printf("Command stop %s\n",cmd->m_name.c_str());
       
    XTCmd* xtcmd=new XTCmd();
    xtcmd->xluaref=cmd;
    xtcmd->stop=true;
    xtcmd->fire=false;
    xtcmd->start=false;
    commandQueue.push_back(xtcmd);

    
    data_mutex.unlock();
}

void XTLuaDataRefs::XTCommandOnce(xtlua_cmd * cmd){
    data_mutex.lock(); 
    char namec[32]={0};
    sprintf(namec,"%p",cmd);
    std::string name=namec;
                                     
    //if(fireCmds.find(name)==fireCmds.end()){
        printf("Command once %s\n",cmd->m_name.c_str());
       
        XTCmd* xtcmd=new XTCmd();
        xtcmd->xluaref=cmd;
        xtcmd->fire=true;
        xtcmd->stop=false;
        xtcmd->start=false;
        commandQueue.push_back(xtcmd);
        //fireCmds[name]=xtcmd;
    
  
    data_mutex.unlock();
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
    
    for (auto x : stringdataRefs) {
        XTLuaCharArray* val=x.second;
        if(val->get&&!val->set){
           
           int size=XPLMGetDatab(val->ref,NULL,0,0);
            //printf("getting %d is %d\n",val->ref,size);
           if(size>0) {
            std::vector<char> inVals(size);
            XPLMGetDatab(val->ref,&inVals[0],0,size);
            val->value=std::string(inVals.begin(),inVals.end());
             //printf("got %s is %d\n",val->value.c_str(),size);
           } else
            val->value.clear();
            val->get=false;
            
        }
        else if(val->set){ 
            const char * begin = val->value.c_str();
		    const char * end = begin + val->value.size();
		    if(end > begin)
		    {
			    XPLMSetDatab(val->ref, (void *) begin, 0, (int)(end - begin));
		    }
            val->set=false;

        }
        
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
    //externally locked
    for(XTCmd* c:commandQueue){
        xtlua_cmd* cmd=c->xluaref;
        if(cmd == nullptr || cmd->m_cmd == nullptr){
            delete c;
            continue;
        }
        if(c->fire){
            printf("Do Command once %s %p\n",cmd->m_name.c_str(),cmd->m_cmd);
            XPLMCommandOnce(cmd->m_cmd);
        
        }
        if(c->start){
            printf("Do Command Begin %s %p\n",cmd->m_name.c_str(),cmd->m_cmd);
            XPLMCommandBegin(cmd->m_cmd);
        }
        if(c->stop){
            printf("Do Command End %s %p\n",cmd->m_name.c_str(),cmd->m_cmd);
            XPLMCommandEnd(cmd->m_cmd);
        }
        delete c;
    }
    commandQueue.clear();
    std::unordered_map<XPLMDataRef,float> newValues;
    for(XTControlObject* c:controlOverrides){
         
         if(c->srcDref==NULL){ //needs init
            printf("Do Create Override %s\n",c->data.c_str());
            json jData =json::parse(c->data);
            printf("Find src %s\n",jData["srcDref"].get<std::string>().c_str());
            c->srcDref=XPLMFindDataRef(jData["srcDref"].get<std::string>().c_str());
            printf("Find dst %s\n",jData["dstDref"].get<std::string>().c_str());
            c->dstDref=XPLMFindDataRef(jData["dstDref"].get<std::string>().c_str());
            c->dstIndex=-1;
            if(c->dstDref)
			{
				XPLMDataTypeID tid = XPLMGetDataRefTypes(c->dstDref);
                if(tid & (xplmType_FloatArray | xplmType_IntArray))			// AND are array type
				{
                   c->dstIndex =jData["dstIndex"].get<int>();
                }
            }
            if(jData.contains("scale"))
                c->scale=jData["scale"].get<float>();
            else
                c->scale=1.0;
            if(jData.contains("scaledref"))
            {
                 c->scaleDref=XPLMFindDataRef(jData["scaledref"].get<std::string>().c_str());
            } 
            if(jData.contains("min"))
            {
                c->min=jData["min"].get<float>();
            }
            if(jData.contains("max"))
            {
                c->max=jData["max"].get<float>();
            }
            c->minin=jData["minin"].get<float>();
            c->maxin=jData["maxin"].get<float>();
            c->minout=jData["minout"].get<float>();
            c->maxout=jData["maxout"].get<float>();
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
void XTLuaDataRefs::addNavData(int    id,
        int    type,
        float  latitude,
        float  longitude, 
        int    frequency,
        float  heading,
        char * name,char * ident){

        NavAid* navaid=new NavAid(); 
        navaid->id=id;
        navaid->type=type;
        navaid->latitude=latitude;
        navaid->longitude=longitude;
        navaid->frequency=frequency;
        navaid->heading=heading;
        navaid->name=std::string(name);
         navaid->ident=std::string(ident);
        navaid->next=NULL;
        if(lastnavaid==NULL)
            navaids=navaid;
        else 
            lastnavaid->next=navaid;
        lastnavaid=navaid;
}
void XTLuaDataRefs::updateNavDataRefs(){
    int entries=XPLMCountFMSEntries();
    int currentIndex=XPLMGetDestinationFMSEntry();
    int currentView=XPLMGetDisplayedFMSEntry();
    json dVdata = json::array();
    dVdata[0]=currentView+1;
    currentDisplayedEntry=dVdata.dump();
   // printf("currentView XPLMGetDisplayedFMSEntry=%d\n",currentView);
    json nVdata =json::array();
    float lastoutLat=0.0f;
    float lastoutLon=0.0f;
    int lastoutAltitude=0;
    bool hasLastOutput=false;
    int count=0;
    for(int i=0;i<entries;i++){
          XPLMNavType         outType;
          char                outID[32]={0}; 
          XPLMNavRef          outRef=XPLM_NAV_NOT_FOUND;
          int                 outAltitude;
          float               outLat;
          float               outLon;
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
    
    
    
    incomingFMSString=nVdata.dump();
    
    if(navaids==NULL){
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
                char                outID[32];    /* Can be NULL */
                char                outName[256];    /* Can be NULL */
                char                outReg[1];
                XPLMGetNavAidInfo(nAid,&outType,&outLatitude,&outLongitude,&outHeight,&outFrequency,&outHeading,outID,outName,outReg);
                /*double latDif=outLatitude-lat;
                double lonDif=outLongitude-lon;
                if(outType!=512&&latDif<2&&latDif>-2&&lonDif<2&&lonDif>-2)
                //if(outType!=512)
                    printf("%d=%d,%d,%f,%f,%f,%s\n",nAid,outType,outFrequency,latDif,lonDif,outHeading,outName); */
                if(outType!=512)
                    addNavData(nAid,outType,outLatitude,outLongitude,outFrequency,outHeading,outName,outID);
                nAid=XPLMGetNextNavAid(nAid); 
            }
    }
    lat=XPLMGetDatad(latR);
    lon=XPLMGetDatad(lonR);
}
bool firstPass=true;
void XTLuaDataRefs::update_localNavData(){
    if(skipNaviads)
        return;
    if(current_navaid==NULL){
        current_navaid=navaids;
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
            

                incomingNavaidString=nVdata.dump();//printf("erasing %d\n",left.size());
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
            skipNaviads=true;
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
    // Scalar DataRefs retain the existing synchronized path. Arrays are
    // snapshotted and flushed separately without holding data_mutex for SDK I/O.
    const bool allGet = ((simTime - beginFlightTime) < 10);
    auto changedList = changeddataRefs;
    changeddataRefs.clear();
    if(allGet)
        changedList = floatdataRefs;
    for(const auto& entry : changedList)
    {
        const std::string& name = entry.first;
        if(arrayDataRefs.find(name) != arrayDataRefs.end())
            continue;
        auto found = floatdataRefs.find(name);
        if(found == floatdataRefs.end() || found->second.empty())
            continue;
        XTLuaArrayFloat* val = found->second[0];
        if((val->get || allGet) && !val->set)
        {
            double fresh = XPLMGetDataf(val->ref);
            if(val->type == xplmType_Double)
                fresh = XPLMGetDatad(val->ref);
            else if(val->type == xplmType_Int)
                fresh = XPLMGetDatai(val->ref);
            val->get = false;
            val->value = static_cast<float>(fresh);
        }
        else if(val->set)
        {
            const float value = static_cast<float>(val->value);
            if(val->type == xplmType_Double)
                XPLMSetDatad(val->ref, value);
            else if(val->type == xplmType_Float)
                XPLMSetDataf(val->ref, value);
            else if(val->type == xplmType_Int)
                XPLMSetDatai(val->ref, static_cast<int>(std::lround(value)));
            val->set = false;
        }
    }
}

double XTLuaDataRefs::XTGetElapsedTime(){
    //data_mutex.lock();
    double retVal=timeT;
    //data_mutex.unlock();
    return retVal;

}
void XTLuaDataRefs::refreshAllDataRefs(){
    struct StringRefresh {
        std::string name;
        XPLMDataRef ref;
        std::string before;
        std::string after;
    };
    struct ScalarRefresh {
        std::string name;
        XPLMDataRef ref;
        int type;
        double before;
        double after;
    };
    std::vector<StringRefresh> strings;
    std::vector<ScalarRefresh> scalars;
    std::vector<std::string> arrayNames;
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        for(const auto& entry : stringdataRefs)
        {
            const XTLuaCharArray* val = entry.second;
            if(val != nullptr && !val->set)
                strings.push_back({entry.first, val->ref, val->value, {}});
        }
        for(const auto& entry : floatdataRefs)
        {
            const std::string& name = entry.first;
            if(arrayDataRefs.find(name) != arrayDataRefs.end())
            {
                arrayNames.push_back(name);
                continue;
            }
            if(entry.second.empty())
                continue;
            XTLuaArrayFloat* val = entry.second[0];
            if(val->set)
                continue; // Preserve a worker write queued before aircraft refresh.
            scalars.push_back({name, static_cast<XPLMDataRef>(val->ref),
                               val->type, val->value, 0.0});
        }
    }
    // Accessor callbacks may run arbitrary provider code. Never invoke them
    // while holding the worker cache lock.
    for(StringRefresh& item : strings)
    {
        const int size = XPLMGetDatab(item.ref, NULL, 0, 0);
        if(size > 0)
        {
            std::vector<char> bytes(static_cast<size_t>(size));
            const int copied = XPLMGetDatab(item.ref, bytes.data(), 0, size);
            item.after.assign(bytes.data(),
                              static_cast<size_t>((std::max)(0, (std::min)(copied, size))));
        }
    }
    for(ScalarRefresh& item : scalars)
    {
        if(item.type == xplmType_Double)
            item.after = XPLMGetDatad(item.ref);
        else if(item.type == xplmType_Int)
            item.after = XPLMGetDatai(item.ref);
        else
            item.after = XPLMGetDataf(item.ref);
    }
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        for(const StringRefresh& item : strings)
        {
            auto found = stringdataRefs.find(item.name);
            if(found == stringdataRefs.end() || found->second == nullptr)
                continue;
            XTLuaCharArray* val = found->second;
            if(val->ref == item.ref && !val->set && val->value == item.before)
            {
                val->value = item.after;
                val->get = false;
            }
        }
        for(const ScalarRefresh& item : scalars)
        {
            auto found = floatdataRefs.find(item.name);
            if(found == floatdataRefs.end() || found->second.empty())
                continue;
            XTLuaArrayFloat* val = found->second[0];
            if(val->ref == item.ref && val->type == item.type &&
               !val->set && val->value == item.before)
            {
                val->value = static_cast<float>(item.after);
                val->get = false;
            }
        }
    }
    for(const std::string& name : arrayNames)
        updateArrayDataRef(name);
}
void XTLuaDataRefs::updateDataRefs(){
    std::vector<std::string> activeArrayNames;
    data_mutex.lock();
    //printf("updateDataRefs\n");
        //timeT = XPLMGetElapsedTime();
        isPaused=XPLMGetDatai(paused_ref);
        simTime=XPLMGetDataf(sim_time_ref);
        //printf("time now %f %f\n",timeT,simTime);
        timeT = simTime;
        if(updateRoll==0&&(XPLMGetDatai(replay_ref) == 0))
        {
            updateNavDataRefs();
            
        }
        else if(updateRoll==5)
        {
            updateStringDataRefs();
            updateRoll=-1;//will go back to 0 next line
        }
        //else if(updateRoll==2)
        {
            updateFloatDataRefs();
            updateCommands(); //always do command queue
            
        } 
        updateRoll++;
        serialWindow.show();
        for(const auto& array : arrayDataRefs)
            if(array.second.active)
                activeArrayNames.push_back(array.first);
    data_mutex.unlock();
    for(const std::string& name : activeArrayNames)
        updateArrayDataRef(name);
}
void XTLuaDataRefs::cleanup(){
    data_mutex.lock();
    drefResolveQueue.clear();
    cmdResolveQueue.clear();
    cmdHandlerResolveQueue.clear();
     for(XTCmd* command:commandQueue){
        delete command;
     }
     commandQueue.clear();
     for (auto x : floatdataRefs) {
        //int i=0;
        string name=x.first;
        std::vector<XTLuaArrayFloat*> val=floatdataRefs[name];
        for(XTLuaArrayFloat* k:val){
            delete k;
        }
     }
     floatdataRefs.clear();
     arrayDataRefs.clear();
     deferredArrayWrites.clear();
     for (auto x : stringdataRefs) {
       // int i=0;
        string name=x.first;
        XTLuaCharArray* val=stringdataRefs[name];
        delete val;
        
     }
     stringdataRefs.clear();
     NavAid* cNav=navaids;
     while(cNav!=NULL){
         navaids=navaids->next;
         delete cNav;
         cNav=navaids;
     }
     for(XTControlObject* c:controlOverrides){
         delete c;
     }

     navaids=NULL;
     lastnavaid=NULL;
     current_navaid=NULL;
     changeddataRefs.clear();
     localNavaids.clear();
     controlOverrides.clear();
     updateRoll=0;
     printf("XTLua:Cleaned up data\n");
    data_mutex.unlock();
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
    int retVal=0;
    data_mutex.lock();
    if(paused_ref==NULL)
        paused_ref=XPLMFindDataRef("sim/time/paused");
    if(replay_ref==NULL)
        replay_ref=XPLMFindDataRef("sim/time/is_in_replay");    
    if(sim_time_ref==NULL){
        sim_time_ref = XPLMFindDataRef("sim/time/total_running_time_sec");  
        beginFlightTime=XPLMGetDataf(sim_time_ref);
    }
    //printf("XTLua:Resolving queue\n");
    for(xtlua_dref * d:drefResolveQueue){
        //printf("Resolving dref %s\n",d->m_name.c_str());
        if(d->m_name.rfind("xtlua/", 0) == 0){
            d->m_types =xplmType_Data;
            continue;
        }
        assert(d->m_dref == NULL);
        assert(d->m_types == 0);
        assert(d->m_index == -1);
        assert(d->m_ours == 0);
        grabLocal(d);
        /*if(d->m_ours){
            printf("Resolved local dref %s\n",d->m_name.c_str());
            continue;
        }*/
       // if(!d->m_ours)
        //    printf("Not local dref %s\n",d->m_name.c_str());
        d->m_dref = XPLMFindDataRef(d->m_name.c_str());
        //initialise our datasets
        if(d->m_dref)
        {
            d->m_index = -1;
            d->m_types = XPLMGetDataRefTypes(d->m_dref);
            // Multiple worker modules may bind the same SDK DataRef. Reuse one
            // snapshot; appending a second array would double its visible length.
            char existingName[32] = {0};
            sprintf(existingName, "%p", d->m_dref);
            if(floatdataRefs.find(existingName) != floatdataRefs.end() ||
               stringdataRefs.find(existingName) != stringdataRefs.end())
            {
                retVal++;
                continue;
            }
            //printf("Resolved dref %s to %p as %d\n",d->m_name.c_str(),d->m_dref,d->m_types);
            if(d->m_types & (xplmType_FloatArray | xplmType_IntArray))			// an array type
            {
                char namec[32];
                sprintf(namec,"%p",d->m_dref);
                std::string name=namec;

                int size=0;
                int type=xplmType_FloatArray;
                if(!(d->m_types & xplmType_FloatArray)){
                    type=xplmType_IntArray;
                    
                    size=XPLMGetDatavi(d->m_dref,NULL,0,0);
                    //printf("Resolved int array dref %s to %p with %d\n",d->m_name.c_str(),d->m_dref,size);
                }
                else{
                    
                    size=XPLMGetDatavf(d->m_dref,NULL,0,0);
                    //printf("Resolved float array dref %s to %p with %d\n",d->m_name.c_str(),d->m_dref,size);
                }
                if(size < 0)
                    size = 0;
                
                std::vector<float> inVals(static_cast<size_t>(size));
                std::vector<int> inIVals(static_cast<size_t>(size));
                if(size > 0)
                {
                    const int copied = !(d->m_types & xplmType_FloatArray)
                        ? XPLMGetDatavi(d->m_dref, inIVals.data(), 0, size)
                        : XPLMGetDatavf(d->m_dref, inVals.data(), 0, size);
                    // A provider may resize between the length query and read.
                    // Publish only elements actually returned by the SDK.
                    size = (std::max)(0, (std::min)(copied, size));
                }

                // Even a zero-length SDK array needs a bridge entry.
                floatdataRefs.emplace(name, std::vector<XTLuaArrayFloat*>{});
                arrayDataRefs[name] = {d->m_dref, type, false};
                
                for(int i=0;i<size;i++){
                    XTLuaArrayFloat* v=new XTLuaArrayFloat;
                    if(!(d->m_types & xplmType_FloatArray))
                        v->value= inIVals[i];
                    else
                        v->value=inVals[i];
                   // printf("defaulted array dref %s to %f with %d\n",d->m_name.c_str(),v->value,size);  
                    v->ref=d->m_dref;
                    v->type=type;
                    v->get=true;
                    v->index=i;
                    //printf("%d=%f\n",i,inVals[i]);
                    //newval.values.push_back(v);
                    floatdataRefs[name].push_back(v);
                }
                //floatdataRefs[name]=newval;
            }
            else if(d->m_types & xplmType_Float || d->m_types & xplmType_Double || d->m_types & xplmType_Int){
                char namec[32];
                sprintf(namec,"%p",d->m_dref);
                std::string name=namec;
                float val=0.0;
                XTLuaArrayFloat* v=new XTLuaArrayFloat;
                if(d->m_types & xplmType_Double){
                    val= static_cast<float>(XPLMGetDatad(d->m_dref));
                    v->type=xplmType_Double;
                }
                else if(d->m_types & xplmType_Float){
                    val=XPLMGetDataf(d->m_dref);
                    v->type=xplmType_Float;
                }
                else if(d->m_types & xplmType_Int){
                    val= static_cast<float>(XPLMGetDatai(d->m_dref));
                    v->type=xplmType_Int;
                }
                v->get=true;
                v->value=val;
                v->ref=d->m_dref;
                //newval.values.push_back(v);
                //printf(" =%f\n",val);
                floatdataRefs[name].push_back(v);

            }else if(d->m_types & xplmType_Data){
                //its a string!
                char namec[32];
                sprintf(namec,"%p",d->m_dref);
                std::string name=namec;
                XTLuaCharArray* v=new XTLuaCharArray;
                v->get=true;
                v->value="";
                v->ref=d->m_dref;
                stringdataRefs[name]=v;
            }
            retVal++;
        }
        else
        {
            std::string::size_type obrace = d->m_name.find('[');
            std::string::size_type cbrace = d->m_name.find(']');
            if(obrace != d->m_name.npos && cbrace != d->m_name.npos)			// Gotta have found the braces
            if(obrace > 0)														// dref name can't be empty
            if(cbrace > obrace)													// braces must be in-order - avoids unsigned math insanity
            if((cbrace - obrace) > 1)											// Gotta have SOMETHING in between the braces
            {
                std::string number = d->m_name.substr(obrace+1,cbrace - obrace - 1);
                std::string refname = d->m_name.substr(0,obrace);
                
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
                            char namec[32];
                            sprintf(namec,"%p",d->m_dref);
                            std::string name=namec;
                            printf("Resolved singleton dref %s to %p\n",d->m_name.c_str(),d->m_dref);

                        }
                    }
                }
            }
        }

    }

    drefResolveQueue.clear();

    for(xtlua_cmd * d:cmdResolveQueue){
        XPLMCommandRef c = XPLMFindCommand(d->m_name.c_str());	
	    if(c == NULL){
		    printf("ERROR: Command %s not found\n",d->m_name.c_str());
	    }
        else
        {
            printf("Resolved Command %s\n",d->m_name.c_str());
            retVal++;
        }
        d->m_cmd = c;
    }
    cmdResolveQueue.clear();

    data_mutex.unlock();
    return retVal;
}
std::vector<xtlua_cmd*> XTLuaDataRefs::XTGetHandlers(){
    std::vector<xtlua_cmd*> retval;
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
float XTLuaDataRefs::XTGetDataf(
                                   xtlua_dref * d,bool local){

    float retVal=0;
    
    if(d->m_ours){
        xlua_dref_ours(d->local_dref);
        retVal= static_cast<float>(xlua_dref_get_number(d->local_dref));
    }
   
    XPLMDataRef  inDataRef=d->m_dref;
    
   // if(!local)
     //   retVal=XPLMGetDataf(inDataRef);
    char namec[32]={0};
    sprintf(namec,"%p",inDataRef);
    std::string name=namec;
    data_mutex.lock();                       
    if(floatdataRefs.find(name)!=floatdataRefs.end()){
        std::vector<XTLuaArrayFloat*> val=floatdataRefs[name];
        if(!d->m_ours){
            val[0]->get=true;
            changeddataRefs[name]=val;
            retVal=val[0]->value;
        }
        else{
            val[0]->value=retVal;
        }
        //floatdataRefs[name]=val;
    }
    else{
        printf("didn't initialise %s\n",name.c_str());


        

    }
    
    data_mutex.unlock();
    return retVal;                                   
}
void  XTLuaDataRefs::XTSetDataf(
                                   xtlua_dref * d,    
                                   float                inValue,bool local)
{
    //printf("set %p=%f\n",inDataRef,inValue) ;
    //if(!local)
     //   XPLMSetDataf(inDataRef,inValue);
     //data_mutex.lock();
     /*if(d->m_ours){
        xlua_dref_ours(d->local_dref);
        xlua_dref_set_number(d->local_dref,inValue);
        //data_mutex.unlock();
        return; 
    }*/
     data_mutex.lock();
     
    XPLMDataRef          inDataRef=d->m_dref;
    char namec[32]={0};
    sprintf(namec,"%p",inDataRef);
    std::string name=namec;
    if(floatdataRefs.find(name)!=floatdataRefs.end()){
        std::vector<XTLuaArrayFloat*> val=floatdataRefs[name];
        if(val[0]->value!=inValue){
            if(!d->m_ours){
                val[0]->set=true;
                changeddataRefs[name]=val;
                val[0]->value=inValue;
            }
            else{
                xlua_dref_set_number(d->local_dref,inValue);
                val[0]->value=inValue;
            }
        }
    }
    else{
        printf("didn't initialise %s\n",name.c_str());

    }

    
    data_mutex.unlock();
}

int XTLuaDataRefs::XTGetDatab(
                                   xtlua_dref * d,    
                                   void *               outValue,    /* Can be NULL */
                                   int                  inOffset,    
                                   int                  inMaxBytes,bool local)
{
    if(inOffset < 0 || inMaxBytes < 0)
        return 0;
    char *outValues =(char *)outValue;
    data_mutex.lock();
    if(d->m_name.rfind("xtlua/controlObject", 0) == 0){
        printf("cant read control object\n");
        data_mutex.unlock();
        return 0;
    }
    if(d->m_name.rfind("xtlua/currentFMS", 0) == 0){
        printf("cant read currentFMS object\n");
        data_mutex.unlock();
        return 0;
    }
    if(d->m_name.rfind("xtlua/xpFMSData", 0) == 0){
        if(outValues!=NULL){
            const char * charArray=currentDisplayedEntry.c_str();
            for(unsigned int i=inOffset;i<currentDisplayedEntry.length()&&i-inOffset<(unsigned int)inMaxBytes;i++){
                outValues[i-inOffset]=charArray[i];
            }
        }
        int retVal=(int)currentDisplayedEntry.length();
        data_mutex.unlock();
        return retVal;
    } 
    if(d->m_name.rfind("xtlua/navaids", 0) == 0){
       // std::string tS="testString";
        //printf("reading navaids %d\n",localNavaidString.length());
         skipNaviads=false;
         if(outValues!=NULL){
             const char * charArray=localNavaidString.c_str();
                for(unsigned int i=inOffset;i<localNavaidString.length()&&i-inOffset<(unsigned int)inMaxBytes;i++){
                    outValues[i-inOffset]=charArray[i];
               }
         }
         else{
             localNavaidString=incomingNavaidString;
         }
         int retVal=(int)localNavaidString.length();
        data_mutex.unlock();
        return retVal;
    }
    if(d->m_name.rfind("xtlua/fms", 0) == 0){
       // std::string tS="testString";
        //printf("reading navaids %d\n",localNavaidString.length());
        skipNaviads=false;
         if(outValues!=NULL){
             
             
             const char * charArray=localFMSString.c_str();
                for(unsigned int i=inOffset;i<localFMSString.length()&&i-inOffset<(unsigned int)inMaxBytes;i++){
                    outValues[i-inOffset]=charArray[i];
               }
         }
         else{
             //if(localFMSString!=incomingFMSString)
             //   printf("FMS=%s\n",incomingFMSString.c_str());
             localFMSString=incomingFMSString;
         }
         int retVal=(int)localFMSString.length();
        data_mutex.unlock();
        return retVal;
    }
    
    /*if(d->m_ours){
        //data_mutex.unlock();
        printf("Our ref XTGetDatab not implimented\n ");
        data_mutex.unlock();
        return 0; 
    }*/
    
    //outValue will be an array of chars
    XPLMDataRef  inDataRef=d->m_dref;
    
    char namec[32];
    sprintf(namec,"%p",inDataRef);
    std::string name=namec;
   // printf("apply XTGetDatab %s[%d] %s\n",name.c_str(),inMaxBytes,outValues!=NULL?"values":"size");
     int retVal=0;
    
    {
        if(outValues!=NULL){
            /*if(stringdataRefs.find(name)!=stringdataRefs.end()){
                XTLuaChars val=stringdataRefs[name];
                for(int i=inOffset;i<val.values.size()&&i-inOffset<inMaxBytes;i++){
                    outValues[i-inOffset]=val.values[i];
                    retVal++;
                }
                val.get=true;
                stringdataRefs[name]=val;
            }*/
            if(stringdataRefs.find(name)!=stringdataRefs.end()){
                XTLuaCharArray* val=stringdataRefs[name];
                const char * charArray=val->value.c_str();
                if(!d->m_ours){
                    val->get=true;
                   
                }
                for(unsigned int i=inOffset;i<val->value.length()&&i-inOffset<(unsigned int)inMaxBytes;i++){
                    outValues[i-inOffset]=charArray[i];
                    retVal++;
                    //printf("apply XTGetDatavf %s %s[%d/%d] %s = %f\n",d->m_name.c_str(),name.c_str(),inOffset,inMax,outValues!=NULL?"values":"size",val[i]->value);
                }
            }
        }
        else
        {
             if(stringdataRefs.find(name)!=stringdataRefs.end()){
                XTLuaCharArray* val=stringdataRefs[name];
                retVal=(int)val->value.size();
                if(!d->m_ours)
                    val->get=true;

            }
            
        }
        
    }
    data_mutex.unlock();
    return retVal;
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
void XTLuaDataRefs::XTSetDatab(
                                   xtlua_dref * d,    
                                   std::string value)
{
    
    //char *inValues =(char *)inValue;
    if(d->m_name.rfind("xtlua/", 0) == 0){
        if(d->m_name.rfind("xtlua/getserial", 0) == 0){
                printf("get serial %s\n",value.c_str());
                serialWindow.init(value);//open a serial entry window
                return;
        }
        else if(d->m_name.rfind("xtlua/currentFMSID", 0) == 0){
            json fPlan=json::parse(incomingFMSString.c_str());
            std::string::size_type sz;   // alias of size_t

            int i_val = std::stoi (value,&sz)-1;
            printf("setting current FMS target to %d\n",i_val);
            XPLMSetDestinationFMSEntry(i_val);
            //
            return;
        }
        else if(d->m_name.rfind("xtlua/loadtoFMS", 0) == 0){
            printf("autoload FMS\n");
            int currentCount=XPLMCountFMSEntries();
            for (int i=currentCount-1;i>=0;i--){    
                XPLMClearFMSEntry(i);
                
            }
            printf("loading %s \n",value.c_str());
            XPLMLoadFMSFlightPlan_f gLoadFMSFlightPlan = (XPLMLoadFMSFlightPlan_f)XPLMFindSymbol("XPLMLoadFMSFlightPlan");
            if (gLoadFMSFlightPlan != NULL) {
                 gLoadFMSFlightPlan(0,value.c_str(), static_cast<int>(strlen(value.c_str())));
            }
            return;
        }
        else if(d->m_name.rfind("xtlua/currentFMS", 0) == 0){
            json fPlan=json::parse(incomingFMSString.c_str());

            printf("setting current FMS target to %s\n",value.c_str());
            for(size_t i=0;i<fPlan.size();i++){
                string cName=fPlan[i][7].get<std::string>();
                printf("current %zu is %s\n",i,cName.c_str());
                if(cName==value){
                    XPLMSetDestinationFMSEntry((int)i);
                    return;
                }
            }
            //
            return;
        }
        else if(d->m_name.rfind("xtlua/controlObject", 0) == 0){
            printf("creating control override object %s\n",value.c_str());
            XTControlObject* override=new XTControlObject();
            override->data=value;
            data_mutex.lock();
            controlOverrides.push_back(override);
            data_mutex.unlock();
            return;
        }
        else if(d->m_name.rfind("xtlua/camera", 0) == 0){
            printf("setting camera %s\n",value.c_str());
            json jcamData=json::parse(value.c_str());
            std::vector<double> thiscamData=jcamData.get<std::vector<double>>();
            bool seenData=false;
            for(int i=0;i<5;i++){
                //printf("setting %d to %f\n",i,(float)thiscamData[i]);
                camData[i]=(float)thiscamData[i];
                if(camData[i]!=0.0)
                    seenData=true;
            }
            if(seenData)
                wantsCamera=1;
            else
                wantsCamera=0;
            if(!controllingCam){
                controllingCam=true;
                XPLMControlCamera(xplm_ControlCameraUntilViewChanges, XTLuaCameraFunc, NULL);  
            } 
            return;
        }
        else if(d->m_name.rfind("xtlua/fltpln", 0) == 0){
            printf("setting flight plan %s\n",value.c_str());
            json fpData=json::parse(value.c_str());
            std::vector<json> waypoints=fpData.get<std::vector<json>>();
            
            int currentCount=XPLMCountFMSEntries();
            printf("%d existing entries\n",currentCount);
            printf("becoming %d entries\n",(int)waypoints.size());
            //int start=currentCount;
            //for (int i=0;i<currentCount&&XPLMCountFMSEntries()>0;i++){
            for (int i=currentCount-1;i>=0;i--){    
                XPLMClearFMSEntry(i);
                //printf("clear to %d existing entries\n",currentCount);
            }
            for(unsigned int i=0;i<waypoints.size();i++){
                std::vector<double> waypoint=waypoints[i].get<std::vector<double>>();
                printf("%d got waypoint %d\n",i,(int)waypoint.size());
                XPLMSetFMSEntryLatLon(i,(float)waypoint[0],(float)waypoint[1], static_cast<int>(std::lround(waypoint[2])));
            }
            printf("got flight plan for %d entries is %d entries\n",(int)waypoints.size(),XPLMCountFMSEntries());
            return;
        }
        else if(d->m_name.rfind("xtlua/xpFMSData", 0) == 0){
             printf("set xtlua/xpFMSData is INOP\n");
             return;
        }
    }
    if(d->m_ours){
        //data_mutex.lock();
        xlua_dref_ours(d->local_dref);
        xlua_dref_set_string(d->local_dref,string(value));
        //data_mutex.unlock();
        //printf("set string %s\n",value.c_str());
        //return; 
    }
    char namec[32];
    XPLMDataRef  inDataRef=d->m_dref;
    sprintf(namec,"%p",inDataRef);
    std::string name=namec;
    data_mutex.lock();
    //printf("apply XTSetDatab %s[%d]\n",d->m_name.c_str(),inLength);
    //if(inValues!=NULL)
    {

            auto valIt=stringdataRefs.find(name);
            if(valIt == stringdataRefs.end() || valIt->second == nullptr){
                data_mutex.unlock();
                return;
            }
            XTLuaCharArray* val=valIt->second;
            val->value=value;
            if(!d->m_ours){
                val->set=true;
                 
            }


    }
    data_mutex.unlock();
}      
int XTLuaDataRefs::XTGetDatavf(
                                   xtlua_dref * d,    
                                   double *             outValues,    /* Can be NULL */
                                   int                  inOffset,    
                                   int                  inMax,bool local)
{
    if(inOffset < 0 || inMax < 0)
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
    if(index < 0)
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
    if(!d || offset < 0 || count < 0)
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
    if(!d || offset < 0)
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


                        
