# XTLua 2.4.9 – Worker/Main-Thread-Grenzen

Diese Änderungen ergänzen den bisherigen, vom Benutzer ohne gefundene Fehler
getesteten Stand. Die Version bleibt **2.4.9**. Dieser neue Stand wurde nicht
kompiliert, nicht nach X-Plane deployt und nicht im Simulator ausgeführt.
Der PC wurde nicht heruntergefahren.

## Architektur bleibt erhalten

`xtlua_worker` führt weiterhin seine Lua-Systemlogik sowie `before_physics()`
und `after_physics()` im eigenen Worker aus. `xtlua_main` und `xlua2_main`
bleiben Main-Thread-Runtimes. Die bestehende SPSC-Dreifachpufferung für
`worker_buffer`/`render_buffer` bleibt unverändert.

Die DataRef-, Command- und Log-Bridges verwenden kurze Mutex-Abschnitte zum
Kopieren bzw. Austauschen ihrer Daten. Sie sind nicht als lock-free deklariert.
SDK-Zugriffe und Lua-Callbacks liegen außerhalb dieser Abschnitte.

## Logging

- `module_log()`, `log_message()` und der XLua-2-Print-Helfer stellen eigene
  Textkopien in eine gemeinsame FIFO-Queue. Keine Lua-/Modulzeiger im Logbatch.
- `xtlua_flush_log_queue()` tauscht den Batch unter einer kurzen Sperre aus;
  erst danach ruft es `XPLMDebugString()` auf.
- Der Drain prüft den beim Pluginstart registrierten Main-Thread und sperrt
  rekursive Drains. Während eines Drains eintreffende Meldungen bleiben im
  nächsten Batch.
- Frame-, Startupfehler-, Disable-, Reload- und Stop-Pfade leeren die Queue;
  beim Cleanup auch nach `lua_close()` und seinen Finalizern.
- Vorhandene `printf`-/Konsole-Ausgaben bleiben bestehen. Das sind keine
  SDK-Aufrufe. Die Queue verwirft keine Einträge wegen eines festen Limits;
  bei dauerhaftem Log-Spam kann ihr Speicherbedarf wachsen.

## Skalare, Strings und Arrays

Für externe SDK-DataRefs gilt nun auch bei Skalaren und Strings:

1. Main-Thread löst das Binding auf und erstellt den initialen Snapshot.
2. Worker liest/schreibt ausschließlich den Cache. Jeder Schreibauftrag
   erhöht eine Versionsnummer, auch derselbe Wert oder ein leerer String.
3. Main-Thread kopiert Wert, Version und SDK-Handle in einen eigenen Batch.
4. SDK-Schreiben und anschließendes Readback erfolgen ohne Cache-Lock.
5. Der Cache übernimmt die Antwort und quittiert den Write nur, wenn die
   Version noch übereinstimmt. Zwischenzeitliche Worker-Writes bleiben dirty.

`refreshAllDataRefs()` nutzt denselben Weg. `updateDataRefs()` hält keinen
äußeren `data_mutex` mehr über die SDK-Arbeit. Das gilt ebenfalls für
`updateStringDataRefs()`, `updateFloatDataRefs()`, `updateNavDataRefs()`,
`updateCommands()` und die Auflösung von Bindings.

Skalare behalten intern Double-Präzision; die Umwandlung zu Float/Int erfolgt
für den jeweiligen SDK-Typ. Strings werden mit einem vollständigen Cache-
Read kopiert, nicht mehr mit getrenntem Längen- und Datenabruf. Die vorhandene
Aktualisierungstaktung bleibt: aktive Arrays/Skalare pro Main-Zyklus,
Strings regulär in jedem sechsten Zyklus sowie beim expliziten Refresh.

XTLua-eigene DataRefs bleiben direkt zugänglicher, geschützter C++-Speicher;
deren Worker-Zugriff benötigt keinen SDK-Roundtrip. Die SDK-Accessor-Seite
verwendet denselben geschützten Speicher. Es gibt keine Transaktion über
mehrere verschiedene DataRefs.

Wichtig bei Strings: `xplmType_Data` ist laut lokalem SDK ein Byte-Array,
keine allgemeine String-/Resize-API. Auch ein leerer Write wird weitergegeben.
Ob ein fremder Provider dadurch verkürzt/leert, bestimmt dessen Accessor.
XTLua fügt keine NUL-Zeichen hinzu und überschreibt keinen unbekannten
Binär-Tail. Bei textbasierten Providern muss das Skript deren Terminator-
Vertrag einhalten. Eigene XTLua-Strings behalten ihre exakte Länge.

## Commands

- Worker → Main: eigene Nachrichten mit Command-Namen und Aktion
  `begin`/`end`/`once`; SDK-Ausführung aus einem abgetrennten Batch.
- Main → Worker: jede empfangene `begin`-, `continue`- und `end`-Phase wird
  mit Handler, Phase und Dauer aufgenommen. Keine 60-Einträge-Grenze,
  keine Phasen-Deduplizierung und kein Zeitstempel-Filter mehr.
- `CommandOnce` darf `begin` und `end` mit identischem Zeitstempel liefern.
  `continue` kommt vom SDK, solange ein Command gehalten wird; es gibt keinen
  zusätzlichen erfundenen `XPLMCommandContinue`-Aufruf.
- Reihenfolge bleibt pro Command erhalten. Bei einem noch unbekannten Namen
  werden dessen Nachrichten aufbewahrt und später erneut versucht. Andere
  Commands dürfen weiterlaufen: ein fehlendes B darf insbesondere `End(A)`
  eines bereits gestarteten A nicht blockieren. Zwischen verschiedenen Namen
  ist die Gesamtfolge deshalb bei fehlenden Commands nicht strikt global.
- Fehlende Commands werden einmal bis zur Auflösung gemeldet. Ein `end` ohne
  eigenen vorherigen `begin` wird mit Diagnose abgewiesen, wie es der SDK-
  Vertrag verlangt. Beim Cleanup werden noch gehaltene Begins ausgeglichen.
- Spät installierte Wrapper werden registriert, ohne bereits installierte
  SDK-Handler erneut einzutragen. Lua-Callbacks laufen außerhalb der Queue-
  Sperre. Während des Dispatch eintreffende Aufträge kommen in einen Folgebatch.

Die Queues sind absichtlich nicht verlustbehaftet begrenzt. Ein dauerhaft
fehlender Command oder ein lange angehaltener Worker kann einen Rückstau
erzeugen. Cleanup verwirft ausstehende Aufträge der beendeten Runtime;
ausgeführte Begins werden zuvor auf dem Main-Thread beendet.

## Timer, FMS, Nav und weitere Hilfswege

- Timer verwenden die vom Main-Thread publizierte Simulationszeit, keine
  Worker-SDK-Abfrage. Fällige Timer werden mit Identität und Revision erfasst.
  Vor dem Callback wird erneut geprüft; Abbrechen, Neuplanen und wiederverwendete
  Speicheradressen lassen keinen alten Eintrag unbeabsichtigt ausführen.
- Timer-Callbacks laufen ohne Timer-Lock. Selbst-Neuplanung gewinnt;
  rekursive Timer-Dispatches werden abgewehrt. Main-/Worker-Listen bleiben
  getrennt, ebenso die per-State-XLua-2-Timer.
- `xtlua/currentFMSID`, `xtlua/currentFMS`, `xtlua/loadtoFMS`, `xtlua/fltpln`,
  `xtlua/camera`, `xtlua/getserial` und `xtlua/controlObject` stellen eigene
  Name-/Nutzdaten-Nachrichten ein. SDK-/UI-Seiteneffekte erfolgen auf Main.
- FMS-/Nav-Abfragen und Positionsdaten werden auf Main erfasst. Die Nav-Liste
  wird vollständig aufgebaut und danach unveränderlich veröffentlicht;
  die lokale Nav-Auswahl und deren JSON-Berechnung bleiben im Worker.
- Kamera-Zustand gehört Main; der Callback füllt auch den Zoom-Wert. Cleanup
  gibt eine noch selbst kontrollierte Kamera frei. FMS-JSON/Kamera-Nutzdaten
  werden vor ihren Seiteneffekten validiert.

## Reload und Lebensdauer

Reload-Commands lösen keinen Cleanup auf einem aktiven Callback-Stack mehr
aus, sondern fordern ihn für die nächste Main-Frame-Grenze an. Aktiver SDK-
oder Timer-Dispatch verhindert eine verschachtelte Abwicklung. XLua-2-
Module verwenden weiterhin den SDK-Plugin-Reload. Ein noch nicht bereiter
Worker-Start wird ebenfalls über vollständigen Plugin-Reload zurückgesetzt.

Eine Mutex/Condition-Variable bestätigt jetzt, dass weder Worker-Laden noch
Lua-Zyklus läuft, bevor Disable/Reload weitergehen. `sleeping` allein wird
nicht als Beweis für Stillstand verwendet. Stop weckt den Worker und joint
ihn vor dem Cleanup. Die Synchronisation betrifft Lifecycle-Übergänge; Main
wartet nicht pro normalem Frame auf die Worker-Berechnung.

Nach Unload-Hooks und Worker-Stillstand schließt `prepare_shutdown()` die
klassischen Bindings. Lua-Finalizer können danach keine bereits freigegebenen
DataRef-/Command-/Timer-Handles verwenden oder neue Bridge-Aufträge anlegen.
Geliehene Worker-DataRef-Aliase werden nicht als eigene SDK-Accessor-Registrierung
abgemeldet; eigene Registrierungen werden nur über ihren gespeicherten Handle
einmal entfernt, nicht durch wiederholte Suche nach möglicherweise fremden Namen.

## Geänderte Dateien und statische Prüfung

- `XTLua/src/shared_xpfuncs.cpp/.h`, `module.cpp/.h`: Logqueue und Closing-State.
- `XTLua/src/xpfuncs.cpp`: gemeinsame Closing-Prüfung der klassischen Bindings.
- `XTLua/src/xpmtdatarefs.cpp/.h`, `xpmtdatatypes.h`: versionierte Übergaben,
  Resolve-Grenze, Commands, FMS-/Nav-/Kamera-/UI-Nachrichten.
- `XTLua/src/xpdatarefs.cpp/.h`: vollständiger String-Read, Binding-Publikation,
  Command-Phasen-/Handler-Synchronisation und geordnetes Cleanup.
- `XTLua/src/xptimers.cpp/.h`: Timer-Identität, Revision und Callback-Grenze.
- `XTLua/src/xlua.cpp`: Log-Drains, Reload-Anforderung und Worker-Pausebestätigung.
- `tests/check_thread_boundary.ps1`: wiederholbare reine Quellcode-Grenzchecks.

Ausführen vom Projektordner:

```powershell
powershell -NoProfile -File tests/check_thread_boundary.ps1
```

Der Check sucht direkte SDK-Aufrufe in ausgewählten Worker-Funktionen,
lexikalische SDK-Aufrufe unter Cache-/Queue-Locks und prüft wesentliche
Logging-/Reload-/Cleanup-Strukturen. Er ist kein Compiler und beweist weder
einen vollständigen transitiven Aufrufgraphen noch Race-Freiheit.
Zusätzlich wurden die Änderungen lokal gegengelesen. Kein externer Review-
Dienst, kein Build, keine Bereitstellung und kein Shutdown wurden ausgeführt.

## Nächste gezielte Simulator-Abnahme

- Mehr als 60 Phasen, gleichzeitige Commands und `CommandOnce`; fehlendes B
  zwischen `Begin(A)`/`End(A)`, später aufgelöstes B und nachträglicher Wrapper.
- Scalar-/String-Write während SDK-Readback, gleicher Wert erneut, leerer/
  kürzerer String, Binär-NUL und Provider mit festen bzw. variablen Längen.
- Fremde Accessors, die während Get/Set erneut XTLua-Commands auslösen.
- Timer A stoppt/plant B neu; Timer plant sich selbst neu; Reload aus Timer
  oder Command; Disable/Enable unmittelbar nach Start bzw. während Laden.
- FMS-/Nav-/Kamera-Helfer unter laufendem Worker, ungültige Nutzdaten,
  Kamera-Kontrollverlust und mehrere aufeinanderfolgende Aufträge.
- Wiederholter Reload/Stop mit gehaltenem Command, Log-Rückstau und Lua-
  Finalizern. Kontrollieren, dass neue Module keine alten Aufträge übernehmen.

Diese Tests sind offen. Die allgemeinen Folgearbeiten stehen in `TODO.md`.
