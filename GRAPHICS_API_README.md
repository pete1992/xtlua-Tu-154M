# Graphics-/ImGui-Ergaenzungen fuer XTLua 2.4.9

Lokale Anpassung an die vorhandenen SDK-4.4-/XLua-Window-APIs und das mitgelieferte
ImGui 1.92.7. Keine Kompilierung und kein Simulator-Test in diesem Arbeitsgang.
Die komplette ImGui-C++-API ist damit nicht als Lua-API abgedeckt.

## Thread- und Fenstervertrag

- `XLuaCreateImguiWindow(config)` / `XLuaDestroyImguiWindow(window)` bleiben
  ausschliesslich `xlua2_main` zugeordnet; die SDK-Konfiguration bleibt gleich.
- Ein Lua-Modul darf jetzt mehrere native ImGui-Fenster besitzen. Jedes Fenster
  besitzt einen eigenen ImGui-Kontext, Font-Atlas, Eingabestatus, Frame-Timer und
  Scratch-Puffer. Ein einzelner Lua-State wird weiterhin nur im Main Thread
  verwendet, nicht zwischen Worker und Rendering geteilt.
- Widgets verwenden den Kontext des aktuell laufenden Fenster-Draw-Callbacks.
  Ein rekursiver zweiter ImGui-Draw-Callback wird nicht betreten.
- Wird ein Fenster aus einem Draw-Callback erzeugt, erfolgt der erste
  Textur-Upload erst im naechsten Main-Thread-Flightloop. Zerstoerung aus einem
  Draw-Callback wird ebenfalls bis nach dem Callback aufgeschoben.
- Der Textur-Flightloop meldet sich niemals aus seinem eigenen Callback ab.
  Ohne Fenster pausiert er ueber den Rueckgabewert und wird beim naechsten
  Fenster reaktiviert; regulaeres Cleanup entfernt die Registrierung.
- Die SPSC-Dreifachpufferung wurde nicht veraendert. Der Worker produziert
  `XTLuaPublishRenderBuffer`; Main kopiert `XLuaGetRenderBuffer` in seinen
  eigenen Lua-State. Keine Grafik-/SDK-Aufrufe im Worker.

## Ergaenzte Widget-Adapter

Der alte Generator ueberspringt mehrere aktuelle ImGui-Signaturen, insbesondere
neue Flags, `ImVec2`-Standardwerte und `int`-Rueckgaben. Deshalb liegen 36
explizite Adapter nach der generierten Registrierung; der Generator und
`imgui_iterator.inl` bleiben unveraendert. Die folgenden Funktionen erfordern
einen aktiven `xlua2_main`-Draw-Callback. Eckige Klammern markieren optionale
Argumente mit ihrem Standardwert.

| Funktion | Lua-Signatur / Rueckgabe |
| --- | --- |
| BeginChild | `(id, [width=0], [height=0], [child_flags=0], [window_flags=0]) -> visible` |
| SetNextWindowPos | `(x, y, [condition=0], [pivot_x=0], [pivot_y=0])` |
| SetNextWindowSize | `(width, height, [condition=0])` |
| SetNextWindowCollapsed | `(collapsed, [condition=0])` |
| BeginCombo | `(label, [preview=""], [flags=0]) -> open` |
| BeginListBox | `(label, [width=0], [height=0]) -> visible` |
| Selectable | `(label, [selected=false], [flags=0], [width=0], [height=0]) -> clicked` |
| BeginTable | `(id, columns, [flags=0], [width=0], [height=0], [inner_width=0]) -> visible`; 1..511 Spalten |
| TableNextRow | `([flags=0], [minimum_height=0])` |
| TableSetupColumn | `(label, [flags=0], [width_or_weight=0], [user_id=0])` |
| BeginTabBar | `(id, [flags=0]) -> visible` |
| BeginTabItem | `(label, [open=nil], [flags=0]) -> selected[, open]` |
| TreeNodeEx | `(label, [flags=0]) -> open` |
| CollapsingHeader | `(label, [flags=0]) -> open` |
| SetNextItemOpen | `(open, [condition=0])` |
| DragFloat / DragInt | `(label, value, [speed=1], [min=0], [max=0], [format="%.3f"/"%d"], [flags=0]) -> changed, value` |
| SliderFloat / SliderInt | `(label, value, min, max, [format="%.3f"/"%d"], [flags=0]) -> changed, value` |
| InputFloat | `(label, value, [step=0], [fast_step=0], [format="%.3f"], [flags=0]) -> changed, value` |
| InputInt | `(label, value, [step=1], [fast_step=100], [flags=0]) -> changed, value` |
| ProgressBar | `(fraction, [width=-FLT_MIN], [height=0], [overlay=nil])` |
| InvisibleButton | `(id, width, height, [flags=0]) -> clicked` |
| IsWindowFocused / IsWindowHovered / IsItemHovered | `([flags=0]) -> boolean` |
| TableGetColumnCount / TableGetColumnIndex / TableGetRowIndex | `() -> integer` |
| Text / TextDisabled / TextWrapped / BulletText / SetTooltip | `(text)` |
| LabelText | `(label, text)` |
| TextColored | `(red, green, blue, alpha, text)` |

Text wird als Text uebergeben, nicht als C-Formatstring. Fuer Formatierung in
Lua `string.format()` verwenden. `imgui.Text("Fuel: 100%")` ist damit sicher.
Die vorhandenen `imgui.constant`-Tabellen liefern Flags und Conditions.

Benutzerdefinierte Zahlenformate von Drag-/Slider-Widgets und `InputFloat`
brauchen genau eine passende Konvertierung: Float `aAeEfFgG`, Integer `diouxX`.
`%%` ist als Prozentzeichen erlaubt. Nicht erlaubt sind `*`, positionale
Argumente, Laengenmodifikatoren, weitere Konvertierungen sowie `%s`/`%n`.
Grenzen: 256 Format-Bytes, Breite 128 und Praezision 32. Slider verlangen
`min <= max`; ihre Grenzen muessen entsprechend der lokalen ImGui-Implementierung
innerhalb der halben Float-/Integer-Typrange liegen. Die neuen Zahlenadapter
weisen nicht-endliche Zahlen und Werte ausserhalb ihres C++-Typs zurueck.
`InputFloat` und `InputInt` unterstuetzen keine Callback-Flags; solche Flags
werden vor dem ImGui-Aufruf als Lua-Argumentfehler abgewiesen.

Fuer `BeginChild` muss `EndChild` **immer** folgen, unabhaengig vom Rueckgabewert.
Fuer `BeginTable`, `BeginCombo`, `BeginListBox`, `BeginTabBar` und `BeginTabItem`
darf das passende `End*` nur nach `true` folgen. `TreeNodeEx` verwendet bei
geoeffnetem Knoten `TreePop`, ausser ein NoTreePush-Flag unterbindet den Push.
`CollapsingHeader` braucht keinen `TreePop`.

## Optimierungen und Korrekturen pro Datei

| Datei | Ergebnis |
| --- | --- |
| `xtlua2_imgui.cpp` | Pro-Fenster-Kontexte; aktive Kontextzuordnung statt Lua-State-Lookup; wiederverwendete Drawcall- und Textpuffer; Textkapazitaeten vor Integer-Verengung validiert; PageUp/PageDown/Insert und Fokuswechsel; aufgeschobene Zerstoerung; Flightloop-Regeln eingehalten. |
| `imgui_lua_bindings.cpp` | 36 gezielte Adapter mit Draw-Grenze; Standardargumente; sichere Text-/Zahlenformatierung, Slider-Grenzen und Numeric-Input-Flags. Keine fremden Pointer oder Frame-/Kontextsteuerung exportiert. `RunString` kehrt bei fehlendem Lua-State sofort zurueck. |
| `SerialWidget.h/.cpp` | Ein gemeinsamer, explizit definierter Singleton statt einer Instanz je Translation Unit; JSON einmal in `init` geprueft, Getter lesen gecachte Strings; native Eintritte gegen Reentrancy/Exceptions abgesichert; wiederholtes Oeffnen und Schliessen raeumen Handles auf; Cleanup-API fuer den Main-Thread; begrenzte String-Lesezugriffe; fehlgeschlagenes `fopen` verursacht keinen Nullzugriff. |
| `xtlua_render_bridge.cpp/.h` | SPSC-Eigentum und Laufzeittrennung geprueft; in diesem Schritt unveraendert. |
| `imgui4xp.cpp`, `imgui_starter_window.cpp`, `ImgWindow/ImgWindow.cpp` | Separater historischer OpenGL-/C++-Fensterpfad, nicht der XLua2-Lua-Fensterhost. Kein pauschaler Wechsel zu PanelGraphics; weiterhin gesonderter Audit-/Migrationskandidat. |

Die globalen `SerialWidget`-Methoden werden im vorhandenen Main-Thread-Flush
aufgerufen; `cleanup()` muss vor Plugin-/Script-Cleanup ebenfalls im Main
Thread erfolgen. Das ist keine GUI-Freigabe fuer Worker-Aufrufe.

## Bewusst offene Grafikfaehigkeiten

`imgui.Image`, `ImageButton` und DrawList-Image-Adapter fehlen weiterhin: der
generierte Binder kennt `ImTextureRef` nicht, und die lokale generierte
SDK-Lua-Schicht exportiert keine allgemeinen CreateTexture/DestroyTexture/
DrawCalls-Wrapper. Die SDK-Dokumentation erlaubt als `tex_ref` ausschliesslich
ein Handle aus `XPLMCreateTexture`, **keine** OpenGL-ID und kein TextureAtlasRef.
Diese Luecke erfordert einen eigenen typisierten Textur-Lebenszyklus; es werden
keine Integer-IDs als vermeintlich kompatible Handles weitergereicht.

Ebenfalls separat: benutzerdefinierte Fonts/Clipboard/IME, ImGui-Docking und
Multi-Viewport, weitere vektor-/callbackbasierte Widgets, individuelle
Cursorformen, exaktes Popout-/VR-Eingabeverhalten. Generierte PanelGraphics-
und native Fensterfunktionen bleiben davon unabhaengig verfuegbar.

## Ausstehende Tests in X-Plane

1. Zwei ImGui-Fenster aus demselben `xlua2_main`-Modul oeffnen; unterschiedliche
   Eingabefelder, IDs, Groessen und Frame-Inhalte muessen unabhaengig bleiben.
2. Zwischen Textfeldern in beiden Fenstern wechseln; Ctrl+A, Backspace,
   PageUp/PageDown, Insert, Enter und Numpad-Enter testen. Das zweite Fenster
   darf den Fokus nicht dauerhaft zurueckstehlen.
3. Fenster aus einem Draw-Callback erzeugen und beide Varianten zerstoeren:
   sich selbst und das andere Fenster. Neue Glyphen nachladen lassen; es darf
   kein Texturzugriff nach Freigabe auftreten.
4. Letztes Fenster aus seinem Draw-Callback schliessen, spaeter ein neues
   erzeugen; Textur-Flightloop muss reaktiviert werden. Danach Reload/Stop.
5. Neue Begin/End-Paare, Tabellen, Tabs, Drag-/Slider-Werte, Prozentzeichen im
   Text und absichtliche Lua-Fehler innerhalb eines Fensters pruefen. Formate
   `%s`, `%n`, `%*f`, `%1$f`, `%lld`, `%f %f`, `%129f` und `%.33f` muessen von
   den passenden neuen Zahlenadaptern abgelehnt werden. Slider mit `min > max`
   oder den vollen `INT_MIN`-/`INT_MAX`-Grenzen ebenfalls. Numeric-Input-
   Callback-Flags testen; gueltige Werte ohne Fehler zurueckgeben lassen.
6. Textbuffer mehrfach vergroessern/verkleinern, leeren Text und 1-MiB-Grenze
   testen; wiederholte Frames muessen denselben Inhalt erhalten.
7. Worker-Renderdaten in beiden Fenstern lesen; Worker darf bei langsamem
   Rendering nicht auf den Main-Thread warten.
8. Serial-Dialog mehrmals anfordern, schliessen, erneut oeffnen und bei
   Reload/Stop aufraeumen; Aktivierung und nicht beschreibbare `serial.bin`
   pruefen. Ungueltiges JSON bzw. fehlende/falsch typisierte dref/title/key-
   Felder duerfen keinen Fehler aus einem nativen Callback entweichen lassen;
   die letzte gueltige Konfiguration bleibt bestehen. Keine alten Widget-
   Handles weiterverwenden.

Statische Quellcode-/Signaturkontrollen ersetzen diese Tests nicht.
Wiederholbare Quellcodekontrollen: `tests/check_graphics_surface.ps1`; das
Skript kompiliert nichts und fuehrt weder Lua noch den Simulator aus.
