cmake_minimum_required(VERSION 3.16)
if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)
endif()

project(XTLua_Project LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------- diagnostics
message(STATUS "Host system    : ${CMAKE_HOST_SYSTEM_NAME}")
message(STATUS "Target system  : ${CMAKE_SYSTEM_NAME}")
message(STATUS "C++ compiler   : ${CMAKE_CXX_COMPILER} (${CMAKE_CXX_COMPILER_ID})")
message(STATUS "Cross compiling: ${CMAKE_CROSSCOMPILING}")

# ---------------------------------------------------------------- paths
set(XTLUA_ROOT     "${CMAKE_CURRENT_SOURCE_DIR}")
set(SDK_DIR        "${XTLUA_ROOT}/../SDK")
set(XLUA_GLUE_DIR  "${SDK_DIR}/XLua_glue")
set(LUA_SDK_DIR    "${XTLUA_ROOT}/../lua_sdk")
set(DEPLOY_DIR     "${XTLUA_ROOT}/../deploy/xtlua/64")

if(NOT EXISTS "${SDK_DIR}/CHeaders/XPLM/XPLMDefs.h")
    message(FATAL_ERROR "X-Plane SDK not found at ${SDK_DIR}")
endif()

if(NOT EXISTS "${XLUA_GLUE_DIR}/XLua_Register_glue.cpp")
    message(FATAL_ERROR "XLua generated glue not found at ${XLUA_GLUE_DIR}")
endif()

# ---------------------------------------------------------------- sources
file(GLOB SOURCE_FILES CONFIGURE_DEPENDS
    "${XTLUA_ROOT}/src/*.cpp"
)

list(APPEND SOURCE_FILES
    
    "${XTLUA_ROOT}/src/ImgWindow/ImgFontAtlas.cpp"
    "${XTLUA_ROOT}/src/ImgWindow/ImgWindow.cpp"
    "${XTLUA_ROOT}/src/imgui_lua_bindings/imgui_lua_bindings.cpp"
    "${XTLUA_ROOT}/src/imgui/imgui.cpp"
	"${XTLUA_ROOT}/src/imgui/imgui_demo.cpp"
    "${XTLUA_ROOT}/src/imgui/imgui_draw.cpp"
    "${XTLUA_ROOT}/src/imgui/imgui_tables.cpp"
    "${XTLUA_ROOT}/src/imgui/imgui_widgets.cpp"
    "${XLUA_GLUE_DIR}/XLua_Register_glue.cpp"
    "${XLUA_GLUE_DIR}/XPLMCamera_glue.cpp"
    "${XLUA_GLUE_DIR}/XPLMDataAccess_glue.cpp"
    "${XLUA_GLUE_DIR}/XPLMDefs_glue.cpp"
    "${XLUA_GLUE_DIR}/XPLMDisplay_glue.cpp"
    "${XLUA_GLUE_DIR}/XPLMGraphics_glue.cpp"
    "${XLUA_GLUE_DIR}/XPLMInstance_glue.cpp"
    "${XLUA_GLUE_DIR}/XPLMMap_glue.cpp"
    "${XLUA_GLUE_DIR}/XPLMMenus_glue.cpp"
    "${XLUA_GLUE_DIR}/XPLMNavigation_glue.cpp"
    "${XLUA_GLUE_DIR}/XPLMPanelGraphics_glue.cpp"
    "${XLUA_GLUE_DIR}/XPLMPlanes_glue.cpp"
    "${XLUA_GLUE_DIR}/XPLMPlugin_glue.cpp"
    "${XLUA_GLUE_DIR}/XPLMProcessing_glue.cpp"
    "${XLUA_GLUE_DIR}/XPLMScenery_glue.cpp"
    "${XLUA_GLUE_DIR}/XPLMSound_glue.cpp"
    "${XLUA_GLUE_DIR}/XPLMUtilities_glue.cpp"
    "${XLUA_GLUE_DIR}/XPLMWeather_glue.cpp"
)

if(NOT SOURCE_FILES)
    message(FATAL_ERROR "No source files found in ${XTLUA_ROOT}/src")
endif()

# ---------------------------------------------------------------- target
add_library(XTLua SHARED ${SOURCE_FILES})

# SDK version defines are cumulative. XPLM440 alone is not sufficient because
# the SDK headers nest newer declarations inside older XPLMxxx guards.
target_compile_definitions(XTLua PRIVATE
    XPLM200=1
    XPLM210=1
    XPLM300=1
    XPLM301=1
    XPLM302=1
    XPLM303=1
    XPLM400=1
    XPLM410=1
    XPLM420=1
    XPLM430=1
    XPLM440=1
    XPLMPG1=1
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS=1
    -DLUA_BINDINGS_LOCAL_STATE
)

target_include_directories(XTLua PRIVATE
    "${XTLUA_ROOT}/src"
    "${XTLUA_ROOT}/include"
    "${XLUA_GLUE_DIR}"
    "${SDK_DIR}/CHeaders/XPLM"
    "${SDK_DIR}/CHeaders/Widgets"
    "${LUA_SDK_DIR}"
    "${XTLUA_ROOT}/src/imgui"
    "${XTLUA_ROOT}/src/imgui/misc/cpp"
    "${XTLUA_ROOT}/src/ImgWindow"
    "${XTLUA_ROOT}/src/imgui/misc/freetype"
)

set_target_properties(XTLua PROPERTIES
    PREFIX ""
    SUFFIX ".xpl"
)

# ---------------------------------------------------------------- lua
set(LUA_51_LIB "${LUA_SDK_DIR}/libluajit.a")

if(NOT EXISTS "${LUA_51_LIB}")
    message(FATAL_ERROR "LuaJIT static library not found: ${LUA_51_LIB}")
endif()

message(STATUS "Using static LuaJIT: ${LUA_51_LIB}")

target_link_libraries(XTLua PRIVATE "${LUA_51_LIB}")

# ---------------------------------------------------------------- platform
if(WIN32)
    # This is the TARGET platform. For a Termux -> Windows cross-build the host
    # is Android/Linux, but XPLM must still see IBM=1.
    target_compile_definitions(XTLua PRIVATE
        APL=0
        IBM=1
        LIN=0
        _CRT_SECURE_NO_WARNINGS
    )

    find_library(XPLM_LIB
        NAMES XPLM_64
        PATHS "${SDK_DIR}/Libraries/Win"
        NO_DEFAULT_PATH
        REQUIRED
    )

    find_library(XPWIDGETS_LIB
        NAMES XPWidgets_64
        PATHS "${SDK_DIR}/Libraries/Win"
        NO_DEFAULT_PATH
    )

    target_link_libraries(XTLua PRIVATE
        "${XPLM_LIB}"
        $<$<BOOL:${XPWIDGETS_LIB}>:${XPWIDGETS_LIB}>
        opengl32
    )

    # MinGW / llvm-mingw runtime: keep the plugin self-contained.
    # Do not use CMAKE_HOST_SYSTEM_NAME here; the host may be Termux/Android.
    if(MINGW OR CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(XTLua PRIVATE
            -static
            -static-libgcc
            -static-libstdc++
        )
    endif()

    set_target_properties(XTLua PROPERTIES OUTPUT_NAME "win")

elseif(APPLE)
    target_compile_definitions(XTLua PRIVATE APL=1 IBM=0 LIN=0)

    find_library(XPLM_FRAMEWORK XPLM
        PATHS "${SDK_DIR}/Libraries/Mac"
        REQUIRED
    )
    find_library(XPWIDGETS_FRAMEWORK XPWidgets
        PATHS "${SDK_DIR}/Libraries/Mac"
    )

    target_link_libraries(XTLua PRIVATE
        "${XPLM_FRAMEWORK}"
        "-framework OpenGL"
        "-framework CoreFoundation"
    )
    target_link_options(XTLua PRIVATE -bundle)
    set_target_properties(XTLua PROPERTIES OUTPUT_NAME "mac")

else() # Linux
    target_compile_definitions(XTLua PRIVATE APL=0 IBM=0 LIN=1)

    target_link_options(XTLua PRIVATE
        -shared
        -rdynamic
        -nodefaultlibs
        -undefined_warning
        -static-libgcc
        -static-libstdc++
    )
    set_target_properties(XTLua PROPERTIES OUTPUT_NAME "lin")
endif()

# ---------------------------------------------------------------- deploy
add_custom_command(TARGET XTLua POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:XTLua>" "${DEPLOY_DIR}/"
    COMMENT "Deploying $<TARGET_FILE_NAME:XTLua> to ${DEPLOY_DIR}"
)

install(TARGETS XTLua
    RUNTIME DESTINATION "${DEPLOY_DIR}"   # Windows .xpl
    LIBRARY DESTINATION "${DEPLOY_DIR}"   # Linux/macOS .xpl
)
