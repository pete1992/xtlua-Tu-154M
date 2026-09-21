//
//  xpmtdatatypes.h
//  xTLua
//
//  Created by Mark Parker on 04/19/2020
//
//	Copyright 2020
//	This source code is licensed under the MIT open source license.
//	See LICENSE.txt for the full terms of the license.

#ifndef xpmtdatatypes_h
#define xpmtdatatypes_h

#include <vector>
#include <string>

class XTLuaArrayFloat{
    public:
    float value=0.0f;
    void * ref=nullptr;
    int type=0;
    int index=-1;
    bool get=false;//do we get this data
    bool set=false;//do we set this data
};

class XTLuaCharArray
{
    public:
    std::string value;
    void * ref=nullptr;
    int index=-1;
    bool get=false;//do we get this data
    bool set=false;//do we set this data
};
class XTLuaChars
{
    public:
    std::vector<char> values;
    void * ref=nullptr;
    //int size;
    //int end;//maybe we dont parse every value
    //int start;//maybe we dont parse every value
    bool get=false;//do we get this data
    bool set=false;//do we set this data
};

#endif /* xpmtdatatypes_h */
