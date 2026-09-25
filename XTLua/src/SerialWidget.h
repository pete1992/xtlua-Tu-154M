#ifndef XTLUA_SERIAL_WIDGET_H
#define XTLUA_SERIAL_WIDGET_H

#include "XPWidgets.h"
#include <cstdint>
#include <string>
 class SerialWidget
{
private:
    bool doShow=false;
    int x=100;
    int y=500;
    int w=350;
    int h=100;
    std::string dataref_name;
    std::string window_title;
    std::string activation_key;
    uint64_t settings_generation = 0;
    
    /*static int	SettingsWidgetsHandler(
						XPWidgetMessage			inMessage,
						XPWidgetID				inWidget,
						intptr_t				inParam1,
						intptr_t				inParam2);*/
public:
    
    SerialWidget();
    void init(const std::string& value);
    void show();
    // Main-thread lifecycle cleanup, before the host plug-in/state is retired.
    void cleanup();
    const std::string& getdRef() const;
    const std::string& getTitle() const;
    const std::string& getKey() const;
};

extern SerialWidget serialWindow;

#endif
