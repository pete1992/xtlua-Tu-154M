-- XLua 2 bootstrap for XTLua's xlua2_main runtime.
-- SDK functions/enums are installed by the host, not by the annotation stubs.
-- Reference: X-Plane/XLua feat/mobile_xlua2, f37b3aec5c36ba7b4ed719b9a7232efa978c0d26.

XPLM_NO_PLUGIN_ID = -1
XPLM_PLUGIN_XPLANE = 0

-- The local SDK registers its real bindings eagerly. Upstream desktop ships
-- annotation-only XPLM*.lua files to satisfy require(); this deployment does
-- not need those stubs. Like upstream's mobile host, use a LAST searcher, so
-- real aircraft-supplied modules keep priority. Only known supported names
-- succeed: misspellings and the unimplemented XPLMSound module still fail.
local sdk_modules = {
    XPLMCamera = true, XPLMDataAccess = true, XPLMDefs = true,
    XPLMDisplay = true, XPLMGraphics = true, XPLMInstance = true,
    XPLMMap = true, XPLMMenus = true, XPLMNavigation = true,
    XPLMPanelGraphics = true, XPLMPlanes = true, XPLMPlugin = true,
    XPLMProcessing = true, XPLMScenery = true, XPLMUtilities = true,
    XPLMWeather = true
}
table.insert(package.loaders, function(name)
    if sdk_modules[name] then
        return function() return true end
    end
    return "\n\tno built-in XTLua SDK module '" .. tostring(name) .. "'"
end)

function run_timer(func, delay, rep)
    local timer = XLuaFindTimer(func)
    if timer == nil then
        timer = XLuaCreateTimer(func)
    end
    XLuaRunTimer(timer, delay, rep)
end

function stop_timer(func)
    local timer = XLuaFindTimer(func)
    if timer ~= nil then
        XLuaRunTimer(timer, -1, -1)
    end
end

function is_timer_scheduled(func)
    local timer = XLuaFindTimer(func)
    return timer ~= nil and XLuaIsTimerScheduled(timer)
end

function get_timer_remaining(func)
    local timer = XLuaFindTimer(func)
    return timer ~= nil and XLuaGetTimerRemaining(timer) or 0
end

function run_at_interval(func, interval)
    run_timer(func, interval, interval)
end

function run_after_time(func, delay)
    run_timer(func, delay, -1)
end

function isnan(value)
    return type(value) == "number" and value ~= value
end
