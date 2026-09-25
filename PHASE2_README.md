# XTLua Phase 2 – lokale Änderungen

Stand: 23. September 2026. Diese Datei beschreibt den lokalen XTLua-Stand,
nicht das öffentliche XLua-Repository. XTLua bleibt eine asynchrone Lua-Runtime
für X-Plane.

## Laufzeitgrenze

| Modus | Ausführung | Grafik und SDK |
| --- | --- | --- |
| xtlua_worker | XTLua-Worker | Berechnung und Worker-Bridge; keine direkten XPLM-/ImGui-Aufrufe |
| xtlua_main | X-Plane-Main-Thread | Bestehende direkte XTLua-/XLua-kompatible Bindings |
| xlua2_main | X-Plane-Main-Thread | Generierte SDK-Bindings, PanelGraphics und ImGui-Fenster |

Die Worker-Lua-Logik erstellt ein worker_buffer als normale Lua-Tabelle.
XTLuaPublishRenderBuffer kopiert dessen Daten in den Worker-Slot des jeweiligen
Kanals und veröffentlicht erst den vollständig aufgebauten Frame. Jeder Kanal
besitzt drei feste, value-owned Slots: worker_buffer, Austausch-Slot und
render_buffer. Der Worker schreibt ausschließlich in worker_buffer; der
X-Plane-Main-Thread übernimmt am Anfang seines Flightloop-Zyklus höchstens den
neuesten fertigen Austausch-Slot als render_buffer. Alle Get-Aufrufe innerhalb
dieses Zyklus lesen denselben Slot und erstellen daraus jeweils ihre eigene
Lua-Tabelle. Der Slot-Tausch verwendet genau ein
std::atomic<uint32_t>; ein static_assert verlangt, dass dieser Typ auf dem
Zielsystem garantiert lock-free ist. Es gibt keine Warteschleife, keinen Mutex
und kein shared_ptr-Atomic in der Render-Bridge. Bei mehreren Publikationen vor
dem nächsten Renderzugriff gilt weiterhin latest wins.
Die Konsistenz gilt pro Kanal; zusammengehörige Daten gehören deshalb in
denselben Kanal und werden als ein Frame veröffentlicht.

Weder Lua-States noch XPLM-Handles oder Zeichenbefehle wandern über die
Thread-Grenze. Die Bridge selbst enthält keine XPLM-Aufrufe.

## Arrays im Worker

Für Worker-Array-DataRefs gibt es zusätzlich zur bisherigen nullbasierten
Einzelindex-Syntax:

    local values = array_ref:get_values()          -- ganzes Array
    local part = array_ref:get_values(4, 8)         -- Offset 4, acht Werte
    local written = array_ref:set_values(values)    -- ganzes Array ab 0
    array_ref:set_values({1.5, 2.5}, 4)             -- zusammenhängender Bereich
    local length = array_ref.len

Offsets sind wie im XPLM nullbasiert; zurückgegebene Lua-Tabellen sind
1-basiert. Die Tabelle für set_values muss eine dichte numerische Folge sein.
Die C++-Bindings heißen XTLuaGetArrayLength, XTLuaGetArrayValues und
XTLuaSetArrayValues. Eine Blockoperation überschreitet die Lua/C++-Grenze
einmal. Auf dem Main-Thread erfasst XPLMGetDatavf bzw. XPLMGetDatavi das
vollständige SDK-Array als Snapshot. Der Worker liest und schreibt nur seinen
lokalen Cache. Ausstehende Writes werden als zusammenhängende Dirty-Bereiche
mit XPLMSetDatavf bzw. XPLMSetDatavi auf dem Main-Thread geschrieben.
Versionsnummern verhindern, dass ein neuerer Worker-Write durch einen
älteren Flush gelöscht wird. Dynamische Array-Längen werden beim nächsten
SDK-Snapshot übernommen; Writes außerhalb einer inzwischen verkleinerten
SDK-Länge bleiben bis zur passenden Länge vorgemerkt.

## Display-Snapshot und ImGui

Im xtlua_worker:

    local sequence, err = XTLuaPublishRenderBuffer("primary", {
        speed = 320.5,
        visible = true,
        caption = "TEST",
        tape = {10, 20, 30}
    })

Im xlua2_main, beispielsweise im drawWindowFunc eines ImGui-Fensters:

    local state, sequence = XLuaGetRenderBuffer("primary")
    if state then
        -- Nur hier auf dem X-Plane-Thread ImGui/PanelGraphics aufrufen.
        -- state.tape ist eine neue, lokale Lua-Tabelle.
    end

Ohne veröffentlichten Frame liefert XLuaGetRenderBuffer nil. Ein Publish
ersetzt den bisherigen Frame des Kanals vollständig; veraltete Zwischenframes
werden nicht als Render-Queue nachgeholt. Erlaubt sind höchstens 32 Kanäle,
128 Felder je Frame, 4096 Werte je numerischem Feld-Array und insgesamt
64 KiB Stringinhalt je Frame. Felder dürfen nur endliche Zahlen, Booleans,
Strings oder dichte numerische Arrays enthalten. Ein abgelehnter Frame
liefert nil und eine Fehlermeldung.

XLuaCreateImguiWindow erwartet im xlua2_main eine Parametertabelle mit
Fenstergeometrie und optionalem drawWindowFunc(window_id, width, height).
XLuaDestroyImguiWindow entfernt das Fenster. Der lokale Pfad erzeugt ein
PanelGraphics-Fenster, hält pro Lua-State einen ImGui-Kontext, leitet Eingaben
auf dem Main-Thread weiter und gibt Fenster, Kontext und Texturen vor dem
Schließen des Lua-States frei. Derzeit ist bewusst höchstens ein ImGui-Fenster
je xlua2_main-Lua-State zulässig. ImGui-Aufrufe sind nur während des aktiven
Draw-Callbacks zulässig; Worker-Lua hat diese Bindings nicht.
Direkte XPLMPanelGraphics-/Avionics-Bindings im xlua2_main bleiben verfügbar.

Die Grafik-Anbindung folgt konzeptionell Laminars
[XLua-2-Fensterhelfer](https://github.com/X-Plane/XLua/blob/f37b3aec5c36ba7b4ed719b9a7232efa978c0d26/src/xlua2_window_helpers.cpp)
und dem
[PanelGraphics-Beispiel](https://github.com/X-Plane/XLua/blob/f37b3aec5c36ba7b4ed719b9a7232efa978c0d26/deploy/example_scripts/PanelGraphicsTestPlugin/PanelGraphicsTestPlugin.lua),
ist aber an die lokale asynchrone XTLua-Architektur und das lokale SDK
angepasst. Sie ersetzt nicht den Worker durch die XLua-Runtime.

## Betroffene Dateien

- XTLua/src/xpdatarefs.cpp/.h, xpfuncs.cpp, xpmtdatarefs.cpp/.h und
  xpmtdatatypes.h: Array-Block-API, Snapshots und Dirty-Bereiche.
- deploy/xtlua/init.lua: Worker-Array-Methoden und dynamische Länge.
- XTLua/src/xtlua_render_bridge.cpp/.h: Worker-/Render-Snapshot-Kanal.
- XTLua/src/xtlua2_imgui.cpp/.h, module.cpp/.h, xlua2_host.cpp und
  imgui_lua_bindings/imgui_lua_bindings.cpp: lokale Main-Thread-Grafik.
- XTLua/src/xlua.cpp: Snapshot-Cleanup beim Stop.
- XTLua/xtlua.vcxproj: neue und bereits lokale Quell-/SDK-Dateien eingetragen.
  Die Projektmappe XTLua/xtlua.sln selbst musste dafür nicht geändert werden.

## Verifikation und offene Grenzen

Der Benutzer meldet für den bisherigen Stand einschließlich der
SPSC-Umstellung bislang keine gefundenen Fehler. Die anschließenden Änderungen
an Logging, skalaren/String-DataRefs, Commands, Timer-/FMS-/Nav-Hilfswegen und
Lifecycle sind in [THREADING_README.md](THREADING_README.md) beschrieben.
Dieser neue Stand wurde auf Wunsch **nicht kompiliert** und nicht im
Simulator ausgeführt. Statische Prüfungen ersetzen keinen Build-/Simulator-
Nachweis.

Skalare und Strings verwenden jetzt versionierte Übergaben; SDK-Aufrufe der
geprüften Update- und Resolve-Pfade liegen außerhalb von data_mutex.
Worker-Logging läuft über eine Main-Thread-Queue. Die kurzen Cache-/Queue-
Sperren bleiben bestehen; nur die Render-Bridge hat den SPSC-lock-free-Vertrag.
Ein Rückkanal für grafische Eingabeereignisse zum Worker ist nicht Bestandteil
dieser Phase. Die Render-Frames werden beim Plugin-Cleanup gelöscht; ein
später Stop-Callback kann deshalb nil sehen.
