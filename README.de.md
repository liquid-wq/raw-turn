# RAW-TURN

RetroAchievements auf einer echten Sega Saturn. Kein Emulator, kein
Disc-Patch.

[English version](README.md) · [Projektseite](https://liquid-wq.github.io/raw-turn/)

![Ein Achievement wird freigeschaltet, während Sonic Jam auf einer echten Saturn läuft](docs/rawturn_unlock.gif)

RAW-TURN verbindet eine echte Sega Saturn, eine SAROO-Kartusche und
RetroAchievements. Eine kleine Ergänzung der SAROO-Firmware liest während des
Spiels die Werte, die ein Achievement-Set braucht, und gibt sie über einen
seriellen Adapter an den PC weiter. Das Programm dort prüft die Achievements
und schaltet sie auf deinem RetroAchievements-Konto frei. Das Spiel selbst
bleibt unverändert.

Wird ein Achievement freigeschaltet, erscheint eine Karte am Bildschirmrand.
In den Optionen lässt sie sich stattdessen im Programmfenster anzeigen.

## Was du brauchst

- Eine SAROO-Kartusche. RAW-TURN wird auf der Platinenversion V1.66
  (STM32H750) entwickelt und getestet. Andere Versionen mit demselben Chip und
  den Pads J2 können funktionieren, sind aber ungetestet. Ältere Platinen mit
  STM32F103 werden nicht unterstützt.
- Eine originale Sega Saturn
- Einen USB-Seriell-Adapter, auf 3,3 V eingestellt
- Windows
- Ein kostenloses RetroAchievements-Konto

Der Adapter kommt an die fünf Pads mit der Aufschrift `G C D T R` auf der
SAROO-Platine, genannt J2: G an Masse, T an RXD des Adapters, R an TXD des
Adapters. C und D bleiben frei. Wo J2 liegt und wie verdrahtet wird, zeigt
[docs/WIRING.md](docs/WIRING.md) (englisch).

![Anschluss des USB-Seriell-Adapters an J2 der SAROO](docs/j2_wiring.svg)

## Los geht's

1. Die neueste Fassung des Programms herunterladen und starten.
2. Die SD-Karte der SAROO in den PC stecken. Unter **SAROO einrichten** auf
   **Firmware und Spiele…** klicken, die Karte wählen und **Firmware patchen**
   klicken.
3. Die Karte zurück in die SAROO und die Saturn einschalten. Ein Firmware
   Update im SAROO-Menü ist nicht nötig.
4. Mit dem RetroAchievements-Konto anmelden. Den seriellen Adapter findet das
   Programm selbst.
5. Ein Spiel starten. Beim ersten Mal bereitet das Programm es vor und schickt
   der SAROO, was sie braucht. Ab dem nächsten Start dieses Spiels zählen die
   Achievements.

Das Programm liefert keine SAROO-Firmware mit. Es nimmt die offizielle
Firmware v0.9 von tpunix, von deiner Karte oder direkt von der Release-Seite
des SAROO-Projekts, und wendet meine Patches auf deinem PC darauf an.
Gepatcht werden nur die beiden Dateien, die die SAROO bei jedem Start von der
Karte lädt, das STM32-Programm und das Saturn-Programm. Der FPGA bleibt der
offizielle. Liegt auf deiner Karte eine andere Firmware, wird sie durch
die gepatchte offizielle ersetzt. Was vorher auf der Karte lag, sichert das Programm
zuerst, und **Original wiederherstellen** holt es zurück.

Im selben Fenster lassen sich auch alle Spiele auf der Karte auf einmal
vorbereiten. Das Programm schreibt dann in die `saroocfg.txt` und sichert sie
vorher.

## Speicherstände

L + R + Unten am Pad legt sofort einen Schnellstand an. Die letzten zehn
Schnellstände bleiben erhalten. Vier weitere Slots und das Laden aller Stände
findest du im Programmfenster unter **Speicherstände**, mit Datum, Uhrzeit und
Spielzeit jedes Stands. Beim Speichern und Laden steht das Spiel einige
Sekunden still.

Laden klappt am besten innerhalb derselben Szene eines Spiels. Ton und
Disc-Zugriff gehören nicht zum Speicherstand.

Im SAROO-Menü gibt es einen neuen Eintrag, **RetroAchievements**. Er zeigt die
vorbereiteten Spiele, einzelne lassen sich dort entfernen.

## Gut zu wissen

Hardcore gibt es noch nicht. Jede Freischaltung wird als Softcore gezählt.

## Aufbau des Repositorys

In `tool/` liegt das Programm. In `saroo/` liegen meine eigenen Ergänzungen
der SAROO-Firmware, für die Saturn und für die STM32. In `docs/` liegen die
Projektseite und die Anschlussanleitung.

## Fehler und Rückmeldungen

Fehler bitte über [GitHub Issues](https://github.com/liquid-wq/raw-turn/issues)
melden, mit Spiel, Hardware und dem Protokoll aus dem Programmfenster. Nur
dort verfolge ich sie.

## Lizenz

RAW-TURN ist kostenlos nutzbar. Der Quelltext ist öffentlich zum Lesen und
Prüfen, aber nicht Open Source — siehe [LICENSE](LICENSE). Fremdkomponenten
stehen in [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).

Es gibt auch eine Fassung für das Mega Drive, [MEGA-RAW](https://github.com/liquid-wq/mega-raw),
und eine für das NES, [RAW-NES](https://github.com/liquid-wq/raw-nes).

Liqui
