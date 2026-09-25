
#include "XPStandardWidgets.h"
#include "XPWidgets.h"
#include "SerialWidget.h"
#include "XPLMDataAccess.h"
#include "XPLMPlugin.h"
#include "json/json.hpp"
#include "shared_xpfuncs.h"
#include <exception>
SerialWidget serialWindow;
static XPWidgetID w_window = nullptr;
static XPWidgetID setButton = nullptr;
static XPWidgetID notificationCaption = nullptr;
static XPWidgetID serialField[6] = {};
static bool serial_operation_active = false;

struct SerialOperationScope {
    SerialOperationScope() { serial_operation_active = true; }
    ~SerialOperationScope() { serial_operation_active = false; }
};

static void serial_error(const char* message) noexcept
{
    // Never log the serial key or let an error while formatting/queueing a
    // diagnostic escape through a native X-Plane callback.
    try { xtlua_queue_log(std::string("XTLua SerialWidget: ") + message + "\n"); }
    catch(...) {}
}

static void close_serial_window()
{
    XPWidgetID window = w_window;
    // Retire handles before SDK destruction: widget callbacks may re-enter.
    w_window = nullptr;
    setButton = nullptr;
    notificationCaption = nullptr;
    for(auto& field : serialField)
        field = nullptr;
    if(window != nullptr)
        XPDestroyWidget(window, 1);
}
SerialWidget::SerialWidget(){

}
static int serialwindowHandler(
						XPWidgetMessage			inMessage,
						XPWidgetID				inWidget,
						intptr_t				inParam1,
						intptr_t				inParam2);
void SerialWidget::init(const std::string& value){
    try {
        const auto settings = nlohmann::json::parse(value, nullptr, false);
        if(!settings.is_object() || !settings.contains("dref") ||
           !settings["dref"].is_string() || !settings.contains("title") ||
           !settings["title"].is_string() || !settings.contains("key") ||
           !settings["key"].is_string()) {
            serial_error("invalid dialog configuration; expected dref/title/key strings");
            return;
        }
        // Stage all potentially allocating work before publishing a new
        // configuration. A rejected request preserves the previous dialog.
        std::string next_dataref = settings["dref"].get<std::string>();
        std::string next_title = settings["title"].get<std::string>();
        std::string next_key = settings["key"].get<std::string>();
        dataref_name.swap(next_dataref);
        window_title.swap(next_title);
        activation_key.swap(next_key);
        ++settings_generation;
        doShow=true;
    } catch(const std::exception&) {
        serial_error("could not prepare dialog configuration");
    }
}
const std::string& SerialWidget::getdRef() const {
    return dataref_name;
} 
const std::string& SerialWidget::getTitle() const {
    return window_title;
}  
const std::string& SerialWidget::getKey() const {
    return activation_key;
}                   
void SerialWidget::show(){
    if(doShow==false || serial_operation_active)
        return;
    SerialOperationScope operation;
    try {
    doShow=false; 
    const uint64_t generation = settings_generation;
    const std::string dataref = getdRef();
    const std::string title = getTitle();
    std::string serial = getKey();
    close_serial_window();
    if(generation != settings_generation) return;
    XPLMDataRef sDref = XPLMFindDataRef (dataref.c_str());
    if(sDref!=NULL){
        XPLMSetDatab(sDref,(void *)serial.c_str(),0,(int)serial.size());
        if(generation != settings_generation) return;
        char activation_Text[1024]={0};
        //nt size=
        XPLMGetDatab(sDref,activation_Text,0,sizeof(activation_Text) - 1);
        //printf("startup is activated text=%s\n",activation_Text);
        if((std::string(activation_Text)).compare("true")==0){
             return;
        }
    }
    else
        return;    
    if(generation != settings_generation) return;
    int x2 = x + w;
	int y2 = y - h;
    w_window = XPCreateWidget(x, y, x2, y2,
					1,	// Visible
					"Enter Serial Number",	// desc
					1,		// root
					NULL,	// no container
					xpWidgetClass_MainWindow);
    if(w_window == nullptr)
        return;
    XPSetWidgetProperty(w_window, xpProperty_MainWindowHasCloseBoxes, 1);
    XPSetWidgetProperty(w_window, xpProperty_MainWindowType, xpMainWindowStyle_Translucent);
    XPAddWidgetCallback(w_window, serialwindowHandler);  
    setButton = XPCreateWidget(x2-80, y-70, x2-20, y-90,
					1, "Save", 0, w_window,
					xpWidgetClass_Button);
    if(setButton == nullptr)
    {
        close_serial_window();
        return;
    }
    XPSetWidgetProperty(setButton, xpProperty_ButtonType, xpPushButton); 

    for(int i=0;i<6;i++){
        serialField[i] = XPCreateWidget(x+10+i*50, y-45, x+50+i*50, y-65,
                        1, "", 0, w_window,
                        xpWidgetClass_TextField);
        if(serialField[i] == nullptr)
        {
            close_serial_window();
            return;
        }
        XPSetWidgetProperty(serialField[i], xpProperty_TextFieldType, xpTextEntryField);
        XPSetWidgetProperty(serialField[i], xpProperty_Enabled, 1);  
    } 
    std::string notifyText=title+" Not Activated";
    notificationCaption =XPCreateWidget(x+10, y-70, x+160, y-100,
					1, notifyText.c_str(), 0, w_window,
					xpWidgetClass_Caption);  
    if(notificationCaption == nullptr)
    {
        close_serial_window();
        return;
    }
    XPSetWidgetProperty(notificationCaption, xpProperty_CaptionLit, 1);
    } catch(const std::exception&) {
        close_serial_window();
        serial_error("could not show dialog");
    }
}

void SerialWidget::cleanup()
{
    doShow = false;
    ++settings_generation;
    close_serial_window();
    dataref_name.clear();
    window_title.clear();
    activation_key.clear();
}

int serialwindowHandler(
					XPWidgetMessage			inMessage,
					XPWidgetID				inWidget,
					intptr_t				inParam1,
					intptr_t				/*inParam2*/){

    if(serial_operation_active)
        return 0;
    SerialOperationScope operation;
    try {

	if (inMessage == xpMessage_CloseButtonPushed)
	{
		close_serial_window();

		return 1;
	}
    if (inMessage == xpMsg_PushButtonPressed && w_window != nullptr &&
        inParam1 == reinterpret_cast<intptr_t>(setButton))
	{
        std::string serial;
        for(int i=0;i<6;i++){
            char buffer[255]={0};//,jvmBuffer[255];
            XPGetWidgetDescriptor(serialField[i], buffer, sizeof(buffer) - 1);
            std::string str(buffer);
            serial+=str+"-";
        }
        //printf("%s\n%s\n",serialWindow.getdRef().c_str(),serial.c_str());
        XPLMDataRef sDref = XPLMFindDataRef (serialWindow.getdRef().c_str());
        if(sDref!=NULL){
            XPLMSetDatab(sDref,(void *)serial.c_str(),0,(int)serial.size());
            char activation_Text[1024]={0};
            //int size=
            XPLMGetDatab(sDref,activation_Text,0,sizeof(activation_Text) - 1);
            //printf("is activated text=%s\n",activation_Text);
            if((std::string(activation_Text)).compare("true")==0){
                close_serial_window();
                FILE *fptr;
                char path_to_me_c[2048];
                XPLMGetPluginInfo(XPLMGetMyID(), NULL, path_to_me_c, NULL, NULL);
                
                // Plugin base path: pop off two dirs from the plugin name to get the base path.
                std::string plugin_base_path=std::string(path_to_me_c);
                std::string::size_type lp = plugin_base_path.find_last_of("/\\");
                plugin_base_path.erase(lp);
                lp = plugin_base_path.find_last_of("/\\");
                plugin_base_path.erase(lp+1);
                plugin_base_path+="serial.bin";
                printf("save serial to %s\n",plugin_base_path.c_str());
                fptr=fopen(plugin_base_path.c_str(),"w");
                if(fptr != nullptr)
                {
                    fprintf(fptr,"%s",serial.c_str());
                    fclose (fptr);
                }
                return 1;
            }
        }
        
        if(notificationCaption != nullptr)
            XPSetWidgetDescriptor(notificationCaption,"Failed");
        
        /*char serialbuffer[255]={0};//,jvmBuffer[255];
        XPGetWidgetDescriptor(ipField, ipbuffer, sizeof(ipbuffer));
        if (inParam1 == (intptr_t)setButton)
        {
            settings.setIP(ipbuffer);
            int tmp = (int)XPGetWidgetProperty(audioDeviceField, xpProperty_ScrollBarSliderPosition, NULL);
            settings.testAudioDevice(tmp);
            return 1;
        }*/
    }
    } catch(const std::exception&) {
        serial_error("dialog callback failed");
        return 1;
    }
     return 0;                  
}
