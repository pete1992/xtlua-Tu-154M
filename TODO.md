# XTLua – weiteres Vorgehen nach 2.4.9

Der lokale XLua-Abgleich und dateiweise Optimierungsdurchgang vom 25.09.2026
stehen in XLUA_PARITY_README.md. Implementierte Ergänzungen sind weiterhin
getrennt von offenen Simulator-/Performance-Nachweisen zu behandeln.

Ausgangslage: Der Benutzer meldet für den bisherigen 2.4.9-Stand einschließlich
der SPSC-Umstellung bislang keine gefundenen Fehler. Die danach umgesetzten
Thread-Grenzen sind nur statisch geprüft, nicht neu kompiliert oder im
Simulator getestet; siehe THREADING_README.md. Die Array-Blockübertragung, Render-Snapshots und der
xlua2_main-ImGui-/PanelGraphics-Pfad bleiben die Basis. XTLua bleibt eine
asynchrone Lua-Runtime.

## P0 – Architekturversprechen absichern

- [ ] Testbedingungen des erfolgreichen 2.4.9-Tests festhalten:
  X-Plane-Version, getestete Module, Array-Typen, Fenster/Displays,
  Reload/Enable/Disable und Testdauer.
- [x] Render-Bridge auf eine echte SPSC-Dreifachpufferung
  umstellen. Der Worker schreibt ausschließlich in worker_buffer, veröffentlicht
  nur einen fertigen Slot und der Main-Thread rendert ausschließlich
  render_buffer. Kein gemeinsamer Lua-State, kein XPLM-Aufruf aus der
  Worker-Seite dieser Bridge und kein nicht garantiert lock-freies
  shared_ptr-Atomic als endgültiger Vertrag.
- [ ] Array-Bridge unter Last prüfen und anschließend entscheiden, ob die
  kurzen data_mutex-Phasen genügen oder ebenfalls durch versionierte
  Snapshot-Slots ersetzt werden müssen.
- [ ] Main-Thread- und Worker-Zugehörigkeit zentral erfassen und in
  Debug-Builds an allen Bridge-, XPLM- und ImGui-Einstiegspunkten prüfen.

Abnahme: Worker-Berechnung und Main-Thread-Rendering können gleichzeitig
laufen, ohne aufeinander zu warten; falsche Thread-Nutzung wird im Debug-Build
sofort erkannt.

## P0 – verbleibende direkte Worker-/SDK-Grenzen schließen

- [x] module_log() und log_message() auf eine Worker→Main-Thread-Logqueue
  umstellen; XPLMDebugString darf aus xtlua_worker nicht mehr direkt erreicht
  werden.
- [x] updateStringDataRefs(), updateFloatDataRefs(), updateNavDataRefs() und
  updateCommands() einzeln prüfen und alle XPLM-Aufrufe aus data_mutex-
  Abschnitten herauslösen.
- [x] Für Skalare und Strings dasselbe Muster wie für Arrays verwenden:
  Main-Thread-Snapshot, Worker-Cache, versionierter Writeback, Main-Thread-
  Flush.
- [x] Command-Phasen als geordnete Main-Thread-Nachrichten übertragen, damit
  begin/continue/end weder verloren gehen noch unter einem Worker-Lock
  ausgeführt werden.
- [x] Timer-, FMS- und Nav-Hilfswege auf direkte Worker-XPLM-Aufrufe und
  Reentrancy prüfen.
- [x] Reload an Frame-Grenze verschieben, Worker-Pause synchronisiert
  bestätigen und klassische Bindings vor Lua-Finalizer-/Handle-Cleanup sperren.
- [ ] Gezielte Runtime-Abnahme dieser Änderungen durchführen; erledigte
  Quellcode-Arbeit ist noch kein Simulator-Nachweis.

Abnahme: Eine Quellcode-Suche und Debug-Thread-Assertions zeigen keinen
direkten XPLM-Aufruf mehr aus xtlua_worker. SDK-Accessor-Callbacks laufen nie
unter dem Worker-Cache-Lock.

## P1 – Array-Bridge systematisch testen

- [ ] FloatArray und IntArray jeweils als fremdes SDK-DataRef und als lokal
  erzeugtes XTLua-DataRef testen.
- [ ] Ganze Arrays sowie Teilbereiche mit Offset 0, mittlerem Offset und
  letztem gültigem Element lesen und schreiben.
- [ ] Null-Länge, Wachstum, Schrumpfung und erneutes Wachstum testen.
- [ ] Einen Worker-Write genau während eines Main-Thread-Snapshots provozieren
  und prüfen, dass die Versionslogik keinen neueren Wert verwirft.
- [ ] Mehrere Module an dasselbe DataRef binden und einen einzigen gemeinsamen
  Snapshot sowie konsistente .len-Werte bestätigen.
- [ ] Ungültige Lua-Tabellen, NaN/Inf, Lücken, falsche Indizes und
  Bereichsüberschreitungen auf definierte Fehlermeldungen prüfen.
- [ ] Übertragene Elemente und Bridge-Zeit pro Frame messen; erst anhand dieser
  Daten Limits oder Dirty-Range-Heuristik optimieren.

Abnahme: Eine reproduzierbare Testmatrix deckt Typ, Größe, Resize, Race und
Fehlerfälle ab. Performance-Aussagen beruhen auf Messwerten.

## P1 – Display-/PanelGraphics-Pfad vervollständigen

- [ ] Ein kleines Referenzmodul anlegen:
  xtlua_worker build() → publish/swap → xlua2_main render(render_buffer).
- [ ] Reload, Plugin-Disable/Enable, Aircraft-Wechsel und Shutdown mit offenem
  ImGui-Fenster testen.
- [ ] Lua-Fehler nach imgui.Begin(), Fensterzerstörung im Draw-Callback,
  Tastaturfokus, Return/Numpad-Enter, Maus und Mausrad testen.
- [ ] Font-Atlas, Texture-Create/Destroy und wiederholte Texture-Updates im
  Simulator testen.
- [ ] XPLMPanelGraphics- und Avionics-Lifecycle mit mindestens einem echten
  Display testen, nicht nur mit einem schwebenden ImGui-Fenster.
- [x] Mehrere ImGui-Fenster pro xlua2_main-State mit unabhängigen Kontexten
  implementieren; genaue API siehe GRAPHICS_API_README.md.
- [ ] Multiwindow-Fokus, verzögerte Zerstörung und neue Widgets im Simulator
  testen; Quellcode-/Stubtests ersetzen diese Abnahme nicht.

Abnahme: Das Referenzdisplay berechnet ausschließlich im Worker und zeichnet
ausschließlich im Main-Thread; Reload und Shutdown hinterlassen keine
Callbacks, Fenster, Kontexte oder Texturen.

## P1 – Eingaben vom Main-Thread zum Worker

- [ ] Einen getrennten, begrenzten Main→Worker-Eventpuffer entwerfen.
- [ ] Ereignisse typisieren: Taste, Maus/Touch, Encoder, Softkey und
  benutzerdefinierte Display-Aktion.
- [ ] Reihenfolge, Zeitstempel, Überlaufstrategie und Koaleszierung festlegen.
  Bewegungsereignisse dürfen koalesziert werden; Button-/Phasenereignisse
  dürfen nicht still verloren gehen.
- [ ] Worker-API zum blockfreien Abholen eines Event-Batches bereitstellen.
- [ ] Keine Lua-Funktion und keinen XPLM-Handle über die Thread-Grenze
  transportieren.

Abnahme: Ein Bedienereignis wird auf dem Main-Thread erfasst und im nächsten
Worker-Zyklus geordnet verarbeitet, ohne dass einer der Threads auf den
anderen wartet.

## P2 – API und Diagnose stabilisieren

- [ ] Namen, Rückgabewerte und Fehlerverhalten der Array-, Render- und
  Event-Bridge festschreiben.
- [ ] Bridge-Statistik bereitstellen: veröffentlichte/übersprungene Frames,
  Dirty-Ranges, übertragenes Volumen, Event-Überläufe und maximale Latenz.
- [ ] Render-Kanäle explizit freigeben können, statt sie nur beim globalen
  Cleanup zu entfernen.
- [ ] Optionalen Sequenzvergleich im Consumer dokumentieren, damit
  unveränderte Frames nicht neu aufgebaut werden müssen.
- [ ] Lua-Array-Komfort vervollständigen: Verhalten von #, pairs und ipairs
  festlegen, ohne die nullbasierten XPLM-Indizes zu verschleiern.
- [ ] README und PHASE2_README nach der finalen API zusammenführen bzw.
  Querverweise bereinigen.

Abnahme: Die öffentliche Lua-API ist dokumentiert, diagnostizierbar und kann
ohne Kenntnis der internen Locks korrekt benutzt werden.

## P2 – Release-Gates

- [ ] Sauberer Release- und Debug-Build aus der aktualisierten xtlua.sln.
- [ ] Statische Prüfungen und gezielte Tests für Lua-Stack, Bounds,
  Versionszähler und Lifecycle.
- [ ] Mindestens ein längerer In-Simulator-Test mit aktiven Worker-Systemen,
  Array-Traffic und Display-Rendering.
- [ ] Aircraft-Reload, Plugin-Reload, Disable/Enable und X-Plane-Shutdown
  jeweils protokollieren.
- [ ] Erst nach diesen Gates entscheiden, ob der Stand als letzte 2.4.x-
  Stabilisierung oder als Basis für 2.5.0 bezeichnet wird.

## Bewusst nicht vermischen

- Keine direkte ImGui-, PanelGraphics- oder sonstige XPLM-Nutzung im
  xtlua_worker.
- Keine Übernahme der synchronen XLua-Runtime als Ersatz für XTLua.
- Keine Performance-Versprechen ohne Messung.
- Keine Erweiterung um neue Komfortfunktionen, solange P0-Grenzen noch offen
  sind.
