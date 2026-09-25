# XTLua 2.4.9 – XLua-Abgleich und Optimierungsdurchgang

Stand: 25.09.2026. Nur lokale Quellcode-/Skriptänderungen.
Keine C++-Kompilierung, keine neuen Plugin-Binaries, keine Installation in
X-Plane, kein Herunterfahren. Die gemeldeten erfolgreichen Simulator-Tests
betreffen den vorherigen Stand, nicht diese Änderungen.

## Referenz und Reichweite

Maßgeblich sind der **lokale XTLua-Code und das lokale SDK**. Der lokale Stand
wurde nicht durch ein GitHub-Repository ersetzt.
Zusätzlich wurde Laminar ausschließlich lesend verglichen:

- [XLua-2-Referenz, f37b3aec](https://github.com/X-Plane/XLua/tree/f37b3aec5c36ba7b4ed719b9a7232efa978c0d26)
- [Bootstrap und gemeinsame Lua-Helfer](https://github.com/X-Plane/XLua/blob/f37b3aec5c36ba7b4ed719b9a7232efa978c0d26/deploy/init.lua)
- [XLua-2-Syntax und Grenzen](https://github.com/X-Plane/XLua/blob/f37b3aec5c36ba7b4ed719b9a7232efa978c0d26/deploy/docs/xlua2-syntax.md)
- [Klassischer XLua-Stand, c4a2e4a6](https://github.com/X-Plane/XLua/tree/c4a2e4a6bbf58535bf09e8a29ad1ff725ee4c314)

Ergebnis des lokalen Registrierungsabgleichs: **333 native Lua-Globals** –
299 SDK-Funktionen, 26 Strukturkonstruktoren und 8 Host-Helfer. Dazu kommen
die drei handgeschriebenen ImGui-Textfunktionen. Alle 310 Funktionsblöcke der
lokalen XLua-SDK-Dokumentation sind diesen Implementierungen zugeordnet.
Übersetzungseinheiten sind in Visual Studio und CMake berücksichtigt.
Das ist ein Abdeckungsnachweis, kein Beweis für sämtliche SDK-Fehlerpfade.

## Architektur bleibt unverändert

| Runtime | Ausführung | Schnittstelle |
| --- | --- | --- |
| `xtlua_worker` | Eigener Worker, einschließlich before/after_physics | Cache/Snapshots, versionierte Writes, Nachrichten; keine direkten XPLM-Bindings |
| `xtlua_main` | X-Plane-Thread | Klassische DataRef-/Command-/Timer-Helfer |
| `xlua2_main` | X-Plane-Thread | Generierte SDK-Bindings, PanelGraphics, ImGui |

Die SPSC-Render-Dreifachpufferung wurde nicht verändert. Worker-Daten bleiben
Werte; weder Lua-States noch SDK-Handles werden in Render-Snapshots geteilt.
Synchrone SDK-Ergebnis-Callbacks werden nicht auf den Worker verlegt.

## Ergänzte Fähigkeiten

- Fehlende Datei `deploy/xtlua/init_v2.lua` ergänzt: Plugin-Konstanten,
  Timer-Helfer und korrektes `isnan()`.
- XLua-2-`require("XPLMDataAccess")` und die anderen 15 unterstützten
  SDK-Namensräume funktionieren auch ohne Annotation-Stubs. Die echten
  Bindings sind bereits nativ registriert. Ein letzter Sucher bestätigt
  bekannte Namen; echte Lua-Module behalten Vorrang. Tippfehler und das
  nicht implementierte `XPLMSound` werden nicht still akzeptiert.
- Klassisches `xtlua_main`: `wrap_command()`, `filter_command()`,
  `get_timer_remaining()`, vollständige Array-Blockzugriffe und dynamische
  Arraylänge. `XLuaReplaceCommand` ist der reguläre Name;
  `XlLuaReplaceCommand` bleibt als bisheriger Alias verfügbar.
- Worker: `get_timer_remaining()`, `raw_table()`, `real_table()`,
  vollständige Array-Zuweisungen auch nach verzögerter DataRef-Auflösung.
- Beide klassischen Bootstraps: lokale Hilfsvariablen statt gemeinsamem
  globalem Scratch-Zustand, gemeinsame Array-/Command-Methoden, korrekte
  Rückgaben aus `dofile()`, funktionierende verschachtelte Namespaces und
  zyklussicheres `dump()`.
- Mehrere ImGui-Fenster pro XLua-2-Modul, jeweils mit eigenem Kontext,
  Eingabestatus und Puffern; 36 zusätzliche/abgesicherte Kernwidget-Adapter. Details und genaue
  Signaturen stehen in [GRAPHICS_API_README.md](GRAPHICS_API_README.md).
- `print()` aller drei Runtimes verwendet die Main-Thread-Logqueue und
  respektiert Lua-`tostring` einschließlich `__tostring`.

### Array-Vertrag

```lua
-- Beide klassischen Runtimes; SDK-Offsets bleiben nullbasiert.
values = find_dataref("sim/example/array")
local snapshot = values:get_values()
snapshot[1] = 42
values:set_values(snapshot)
values:set_values({7, 8}, 2) -- schreibt DataRef-Indizes 2 und 3
values = {1, 2, 3}          -- Blockwrite, Bindung bleibt erhalten
```

Lua-Wertetabellen sind dicht und einsbasiert. Auch eine vom klassischen
Namespace gewrappte globale Snapshot-Tabelle wird beim Write entpackt.
`.len` ist die aktuelle Snapshot-/SDK-Länge; `#`, `pairs` und `ipairs`
auf DataRef-Proxies wurden nicht als neue Array-API eingeführt.

Fremde Main-DataRefs verwenden vollständige SDK-Bufferschnittstellen.
Eigene DataRefs greifen direkt unter ihrem Speicher-Lock zu, ohne einen
zusätzlichen Bridge-Lock. Auf dem Worker bleiben Writes versioniert.
Ein vorgemerkter Einzelwrite außerhalb der aktuellen Länge erzeugt keine
riesige Zwischenallokation mehr: er bleibt sparse vorgemerkt und lässt die
sichtbare Länge unverändert, bis ein Main-Snapshot die Position enthält.
Blockwrites müssen weiterhin vollständig in die aktuelle Länge passen.
Der Rückgabewert eines Writes bezeichnet übergebene Elemente, keine
Bestätigung eines fremden DataRef-Providers.

### Command-Vertrag

`filter_command(name, function() return boolean end)` ist nur im
`xtlua_main`-Initpfad verfügbar. Im Worker gibt es dafür eine ausdrückliche
Fehlermeldung. Der erste Begin bestimmt die Entscheidung bis zum Ende der
überlappenden normalen Holds. Begin/End werden getrennt nachverfolgt.
Ein Lua-Fehler oder ein nichtboolesches Filterergebnis lässt den Command
weiterlaufen und erzeugt eine Diagnose.

Die Reihenfolge ist Filter → Pre-Wrapper → Replacement/Simulator →
Post-Wrapper. Bei eigenem Replacement wird Post ausdrücklich aufgerufen,
weil das Replacement die weitere SDK-Verarbeitung unterdrückt.
Phasen und Handlerdaten werden vor Lua-Aufrufen kopiert.

Rekursive Start/Stop/Once-Aufrufe desselben Commands einschließlich Alias
sind aus dessen Handler nicht erlaubt. Indirekte rekursive SDK-Begins
werden als blockierte Holds verfolgt. Wird ein noch nicht weitergeleiteter
Begin während eines Pre-Callbacks indirekt beendet, bricht seine restliche
Weiterleitung ab: kein End vor Begin beim Simulator. Ein bereits beobachteter
Pre-Begin wird dabei nicht künstlich durch einen zusätzlichen Lua-Callback
kompensiert. Pre-Beobachtung ist noch keine Simulator-Bestätigung.

## Dateiweise Prüfung und Optimierung

| Datei unter `XTLua/src` (jeweils .h mitgeprüft) | Ergebnis dieses Durchgangs |
| --- | --- |
| `module.cpp/.h` | SDK-Closing-Gate vor Skriptzugriff, Schutz gespeicherter Funktionsaliase, geschützte SDK-Aufruftiefe für synchrone Lifecycle-Callbacks, gemeinsame Print-Queue, lokaler Message-Traceback, Loader-Fehler ohne normalen Longjmp |
| `lua_helpers.cpp/.h` | Ein zentraler aufruflokaler Traceback, auch für generierte/reentrante Callbacks; SDK-tostring beim Schließen gesperrt |
| `xpfuncs.cpp/.h` | Gemeinsame Array-Validierung, Main-Block-Bindings, Timer-Restzeit/endliche Zeiten, Wrap/Filter, Callback-Validierung vor Persistierung |
| `xpdatarefs.cpp/.h` | Eigene Array-Speicherzugriffe ohne temporären Vektor bei Einzelreads; Main-SDK-Batches; Notifier einmal pro Array-Änderung; Changed-Set per Swap; Stringvergleich ohne Vollkopie; Main-Command-Dispatch |
| `xpmtdatarefs.cpp/.h` | Native Handle-Schlüssel statt formatierter Pointer-Strings, RAII-Locks, sparse vorgemerkte Writes, Normalisierung direkt in den Cache, wiederverwendete aufgelöste Command-Handles |
| `xpmtdatatypes.h` | Geprüft; Double-Werte und Versionsfelder für exakte Int32-/Writeback-Semantik beibehalten |
| `xpcommands.cpp/.h` | Header um Main-Wrap/Filter/Dispatch-Grenze erweitert; .cpp bleibt bewusst leer, Implementierung liegt in xpdatarefs.cpp |
| `xptimers.cpp/.h` | Zentralen Traceback nutzen; vorhandene Identitäts-/Revisionsprüfung, getrennte Runtime-Listen und Snapshot-Uhr beibehalten |
| `shared_xpfuncs.cpp/.h` | Closing-/Lifecycle-Callbackzulassung, Funktion vor Capture prüfen, alte Callback-Registry-Slots beim Ersetzen freigeben, O(1)-Callbackzählung |
| `shared_lua_helpers.h` | Deklarationen geprüft, zentrale Implementierung liegt in lua_helpers.cpp; keine zweite .cpp |
| `xlua_command_bindings.cpp/.h` | Callback vor Capture prüfen, Selbst-Unregister überleben, bei Fehler/Disable weiterreichen, Cleanup vor SDK-Reentry abtrennen |
| `xlua2_host.cpp/.h` | Registrierte Host-Helfer, Timerparameter und Besitzzuordnung geprüft; unnötigen Umbau vermieden |
| `xtlua2_imgui.cpp/.h` | Fensterlokale Kontexte, wiederverwendete Draw-/Textpuffer, Fokus/Tasten, verzögertes Destroy und SDK-konformer Textur-Flightloop |
| `imgui_lua_bindings/imgui_lua_bindings.cpp` | Gezielt ergänzte Widget-Adapter, Draw-Grenze, geprüfte numerische Formate und Sliderbereiche |
| `SerialWidget.cpp/.h` | Ein gemeinsames Objekt statt static pro Übersetzungseinheit; Konfiguration einmal prüfen/parsen; gecachte Strings; sichere Native-Callbacks/Handles |
| `xlua.cpp` | Serial-Dialog in Main-Cleanup integriert; Worker-/Frame-/Reload-Modell und Version 2.4.9 beibehalten |
| `xluaplugin.cpp/.h` | Einstiegspunkte/Main-Zuordnung geprüft; Debugmenü und Plattformpolitik nicht verändert |
| `xtlua_render_bridge.cpp/.h` | Bestehenden SPSC-Vertrag geprüft/beibehalten; keine Änderung am Render-Transport |
| `imgui4xp.*`, `imgui_starter_window.*`, `ImgWindow/*` | Separater historischer OpenGL-Pfad eingeordnet, nicht als neuer XLua-2-Renderer umgebaut |
| ImGui-/JSON-/stb-/Font-Fremdquellen | Nicht pauschal umgeschrieben; benötigte Header/Signaturen gegen Adapter geprüft |

Die drei Lua-Bootstraps wurden separat funktional mit Stubs geprüft.
SDK-generierte Dateien blieben unverändert; gemeinsame Host-Helfer tragen
die querschnittlichen Korrekturen.

Die bisherige harte Grenze von 500 SDK-Captures ist jetzt eine einmalige
Warnung je State. Ein Lua-Fehler nach bereits erfolgter Persistierung konnte
C++-Destruktoren überspringen. Captures werden daher weiter sicher gehalten;
ein belastbares hartes Limit braucht Vorabprüfung der gesamten generierten
Bindung. Auch der allgemeine Invalid-Argument-/Longjmp-Pfad sämtlicher
generierter Bindings bleibt ein eigener Auditpunkt.

## Prüfungen und weitere Schritte

Wiederholbare lokale Tests:

- `tests/check_xlua_surface.ps1`: Registry, Implementierungen, Projekte,
  Bootstrap, SDK-Gate, Tracebacks und Callbackzulassung.
- `tests/check_classic_bridge.ps1`: führt auch
  `check_thread_boundary.ps1` aus; zusätzliche Array-/Command-Verträge.
- `tests/check_graphics_surface.ps1`: Widget-Signaturen, Fensterbesitz,
  Formate und Lifecycle-Grenzen.
- `tests/check_command_filter_model.ps1`: ausdrücklich ein Zustandsmodell,
  kein Test des kompilierten C++.
- `tests/check_lua_bootstrap.lua`: echte Lua-Skriptausführung gegen
  SDK-Stubs; mit vorhandenem LuaJIT und `-joff` geprüft. Das mitgelieferte
  Build-MiniLua besitzt nicht die erforderliche Standardbibliothek.

Keine behauptete Beschleunigung in Prozent, keine neuen Simulatorergebnisse.

Letzter lokaler Prüflauf: 742 Thread-Grenzen-, 58 zusätzliche Classic-Bridge-,
1409 SDK-Surface- und 218 Grafik-Quellcodechecks bestanden. Zusätzlich
40 Command-Modellchecks und 80 Lua-Bootstrap-Stubchecks bestanden.

Die Optimierungen reduzieren strukturell Kopien, Schlüsselkonvertierungen,
Registry-Suchen und Lock-Verschachtelungen; Messwerte stehen aus.

Als Nächstes:

1. Neue Main-Wrap/Filter-, Array- und Multiwindow-Fälle in X-Plane abnehmen.
   Besonders Resize/Writeback während SDK-Callbacks, Fehler und Reload testen.
2. Typisierte Bild-/Textur-Lebensdauer für ImGui-Image-Funktionen ergänzen.
   Keine OpenGL-IDs als PanelGraphics-Texturhandles ausgeben.
3. Main→Worker-Bedienereignisse separat geordnet übertragen; keine
   Lua-Callback- oder SDK-Handle-Weitergabe.
4. Generierte Marshalling-/Invalid-Argument-Pfade sowie Registrierung aus
   Lua-Coroutines einschließlich Thread-Pinning und Cleanup gesondert prüfen.
   Der jetzige Abgleich bestätigt dort nicht sämtliche Lebenszeitfälle.
5. Danach anhand von Simulatorprofilen die verbleibenden DataRef-Locks,
   Snapshot-Allokationen und Renderkopien gezielt weiter optimieren.

Vollständige Gleichheit mit jeder ImGui-C++-Funktion oder mit künftigen XLua-
Vorschauversionen wird nicht behauptet. Die offenen Grafikfähigkeiten und
konkreten Simulator-Testgruppen sind im Grafik-README aufgeführt.
