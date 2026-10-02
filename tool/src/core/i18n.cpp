#include "core/i18n.h"

#include <atomic>
#include <cstring>
#include <string>
#include <unordered_map>

namespace i18n {
namespace {

std::atomic<int> gSprache{(int)Sprache::De};

struct Paar { const char* de; const char* en; };

// Deutsch -> Englisch. Der deutsche Text muss Zeichen fuer Zeichen dem im
// Quelltext entsprechen. Platzhalter wie %s
// und %d muessen in Zahl, Art und Reihenfolge uebereinstimmen.
const Paar kTabelle[] = {
    // --- Hauptfenster -----------------------------------------------------
    {"Optionen", "Options"},
    {"Abbrechen", "Cancel"},
    {"Schließen", "Close"},
    {"über die Katze", "about the cat"},
    {"aber falls du darüber nachdenkst,\nlies bitte zuerst 'über die Katze'",
     "but if you're thinking about it,\nplease read 'about the cat' first"},
    {"Farbschema", "Colour scheme"},
    // --- Speicherstaende ohne Ingame-Menue --------------------------------
    {"Speicherstände", "Save states"},
    {"Slots lesen", "Read slots"},
    {"Speichern", "Save"},
    {"Laden", "Load"},
    {"leer", "empty"},
    {"Im Hardcore-Modus gesperrt.", "Locked in hardcore mode."},
    {"Konsole arbeitet … das Spiel steht so lange still.",
     "Console is working … the game is paused meanwhile."},
    {"L + R + Unten am Pad legt einen neuen Schnellstand X.n an (die letzten 10 bleiben).",
     "L + R + Down on the pad creates a new quick save X.n (the last 10 are kept)."},
    {"Gespeichert in Slot ", "Saved to slot "},
    {"Ingame-Menü im Hardcore-Modus gesperrt.", "In-game menu locked in hardcore mode."},
    {"Das Ingame-Menü ist in diesem Spiel nicht möglich. Speichern mit L + R + Unten oder hier im Werkzeug.",
     "The in-game menu is not available in this game. Save with L + R + Down or here in the tool."},
    {"Slot ", "Slot "},
    {" geladen.", " loaded."},
    {"Speichern in Slot ", "Saving to slot "},
    {"Laden aus Slot ", "Loading slot "},
    {" ging nicht: ", " failed: "},
    {"der Slot ist leer", "the slot is empty"},
    {"im Hardcore-Modus gesperrt", "locked in hardcore mode"},
    {"Fehler beim Zugriff auf die Karte", "error accessing the card"},
    {"unbekannter Auftrag", "unknown request"},
    {"Keine Antwort der Konsole auf den Speicherstand-Auftrag.",
     "No answer from the console to the save state request."},
    {"Speicherstand-Auftrag konnte nicht gesendet werden.",
     "Could not send the save state request."},
    {"Fehler melden", "Report a bug"},
    {"Freischalt-Anzeige testen", "Test unlock notification"},
    {"Ordner wählen und auf Prüfen klicken.", "Choose a folder and click Check."},
    {"Probe-Anzeige", "Test notification"},
    {"So sieht eine Freischaltung aus.", "This is what an unlock looks like."},
    {"Der Link zur Fehlerliste folgt, sobald das Projekt auf GitHub liegt.",
     "The link to the issue tracker will follow once the project is on GitHub."},
    {"Heute", "Current"},
    {"Graphit", "Graphite"},
    {"Saturn-Grau", "Saturn Grey"},
    {"Bernstein", "Amber"},
    {"Hell", "Light"},

    {"RetroAchievements-Login", "RetroAchievements login"},
    {"Benutzername", "Username"},
    {"Passwort", "Password"},
    {"speichern", "remember"},
    {"Anmelden", "Log in"},
    {"Abmelden", "Log out"},
    {"meldet an…", "logging in…"},
    {"angemeldet als ", "logged in as "},
    {"nicht angemeldet", "not logged in"},

    {"SAROO einrichten", "Set up SAROO"},
    {"Firmware und Spiele…", "Firmware and games…"},
    {"Patcht einmalig die Firmware der SAROO auf der SD-Karte. Auf Wunsch richtet es auch "
     "den Hook für einen kompletten ROM-Ordner im Voraus ein. Ohne das ein Spiel zweimal "
     "starten: Beim ersten Start wird der Hook eingetragen, beim zweiten ist er aktiv.",
     "Patches the SAROO firmware on the SD card, once. It can also set up the hook for a "
     "whole ROM folder in advance. Without that, start a game twice: the first start "
     "stores the hook, the second start activates it."},

    // Firmware der SAROO
    {"Firmware der SAROO", "SAROO firmware"},
    {"Spiele", "Games"},
    {"SD-Karte, z. B. H:\\", "SD card, e.g. H:\\"},
    {"Karte wählen…", "Choose card…"},
    {"SD-Karte der SAROO wählen", "Choose the SAROO SD card"},
    {"Dort gibt es keinen Ordner SAROO. Bitte das Laufwerk der SD-Karte wählen.",
     "There is no SAROO folder there. Please choose the drive of the SD card."},
    {"fehlt", "missing"},
    {"offizielle v0.9", "official v0.9"},
    {"gepatcht", "patched"},
    {"andere Fassung", "other version"},
    {"· vorheriger Stand gesichert", "· previous version backed up"},
    {"Freischalt-Anzeige", "Unlock display"},
    {"am Bildschirmrand", "at the screen edge"},
    {"im Werkzeug", "inside the tool"},
    {"Update verfügbar", "Update available"},
    {"Eine neue Version ist verfügbar (Build %d, du hast Build %d).",
     "A new version is available (build %d, you have build %d)."},
    {"Herunterladen", "Download"},
    {"Später", "Later"},
    {"Firmware patchen", "Patch firmware"},
    {"Original wiederherstellen", "Restore original"},
    {"Jetzt die Karte in die SAROO stecken und die Saturn einschalten. Ein "
     "Firmware Update im SAROO-Menü ist nicht nötig.",
     "Now put the card into the SAROO and switch the Saturn on. No firmware update in "
     "the SAROO menu is needed."},
    {"Stelle die Original-Firmware wieder her…", "Restoring the original firmware…"},
    {"Patche die Firmware…", "Patching the firmware…"},
    {"Original-Firmware liegt auf der Karte.", "The original firmware is on the card."},
    {"Firmware gepatcht.", "Firmware patched."},
    {"Kein Ordner für Zwischendateien.", "No folder for temporary files."},
    {"Lade die offizielle SAROO-Firmware v0.9 von der Seite des SAROO-Projekts…",
     "Downloading the official SAROO firmware v0.9 from the SAROO project page…"},
    {"Herunterladen fehlgeschlagen: ", "Download failed: "},
    {"Das heruntergeladene Paket ist nicht die offizielle v0.9.",
     "The downloaded package is not the official v0.9."},
    {"Paket ließ sich nicht zwischenspeichern.", "Could not store the package."},
    {"Entpacken nicht möglich: tar.exe fehlt.", "Cannot unpack: tar.exe is missing."},
    {"Das Paket ließ sich nicht entpacken.", "Could not unpack the package."},
    {"Im Paket fehlt: ", "Missing in the package: "},
    {"Auf diesem Laufwerk gibt es keinen Ordner SAROO.", "There is no SAROO folder on this drive."},
    {"Sicherung fehlgeschlagen: ", "Backup failed: "},
    {"Gesichert: ", "Backed up: "},
    {"Schreiben fehlgeschlagen: ", "Writing failed: "},
    {"Gepatcht: ", "Patched: "},
    {"Aus der Sicherung zurück: ", "Restored from backup: "},
    {"Offizielle v0.9 geschrieben: ", "Official v0.9 written: "},
    {"Patch beschädigt", "Patch damaged"},
    {"Originaldatei passt nicht zum Patch", "Original file does not match the patch"},
    {"Ergebnis stimmt nicht mit der Prüfsumme überein", "Result does not match the checksum"},

    {"Verbindung", "Connection"},
    {"Trennen", "Disconnect"},
    {"sucht…", "searching…"},
    {"SAROO verbinden", "Connect SAROO"},
    {"nicht verbunden", "not connected"},
    {"Anschluss offen, Gerät noch nicht erkannt", "Port open, device not detected yet"},
    {"keine Antwort", "no response"},
    {"SAROO erkannt, kein Hook installiert", "SAROO detected, no hook installed"},
    {"SAROO verbunden, Hook läuft", "SAROO connected, hook running"},
    {"SAROO verbunden, Bildzähler steht", "SAROO connected, frame counter stuck"},

    {"Spiel", "Game"},
    {"frage RetroAchievements…", "asking RetroAchievements…"},
    {"Erkennung meldet die SAROO beim Spielstart selbst",
     "The SAROO reports the game by itself when it starts"},
    {"kein Spiel erkannt", "no game detected"},
    {"Gebiet %s", "Region %s"},
    {"von RetroAchievements nicht unterstützt", "not supported by RetroAchievements"},
    {"MD5 %s  (von der Konsole gemeldet)", "MD5 %s  (reported by the console)"},

    {"Achievements", "Achievements"},
    {"Titel", "Title"},
    {"Punkte", "Points"},
    {"freigeschaltet", "unlocked"},
    {"gesperrt", "locked"},

    {"Live-Werte (letzter WATCH_REPORT):", "Live values (last WATCH_REPORT):"},
    {"tick: %u  ·  %.0f/s  ·  %d CRC-Fehler", "tick: %u  ·  %.0f/s  ·  %d CRC errors"},
    {"  ·  noch keine Änderung", "  ·  no change yet"},
    {"  ·  Änderung vor %.0f s", "  ·  changed %.0f s ago"},
    {"Hier erscheinen die Werte aus dem Spielspeicher, sobald Frames vom Gerät ankommen.",
     "Values from game memory appear here as soon as frames arrive from the device."},
    {"Keine Speicherstellen beobachtet.", "No memory locations watched."},
    {"Der Hook läuft und meldet die Spielkennung, aber es wurde keine Watch-Liste "
     "installiert. Das passiert, wenn es für dieses Spiel kein Achievement-Set gibt — dann "
     "ist auch nichts zu beobachten. Sobald ein Spiel mit Set läuft, stehen hier seine "
     "Speicherstellen.",
     "The hook is running and reports the game ID, but no watch list was installed. This "
     "happens when there is no achievement set for this game — then there is nothing to "
     "watch. As soon as a game with a set is running, its memory locations appear here."},
    {"Log:", "Log:"},

    {"Das Programm ist abgestuerzt.\n\nEinzelheiten stehen in %TEMP%\\saturn_ra_monitor.log",
     "The program crashed.\n\nDetails are in %TEMP%\\saturn_ra_monitor.log"},
    {"OpenGL-Kontext konnte nicht angelegt werden.", "Could not create the OpenGL context."},
    {": noch ohne Funktion, die Logik ist noch nicht angeschlossen.",
     ": not functional yet, the logic is not connected."},
    {"Noch ohne Funktion", "Not functional yet"},
    {"Zuletzt eingerichtet: %d Spiel(e). Erkannt wird trotzdem aus dem laufenden Spiel, "
     "nicht aus dieser Liste.",
     "Last set up: %d game(s). Detection still comes from the running game, not from "
     "this list."},

    // --- Anschlusssuche ---------------------------------------------------
    {"Kein serieller Anschluss gefunden.", "No serial port found."},
    {"Anschluss vorausgewählt: %s (%s).", "Port preselected: %s (%s)."},
    {"%d Anschluss/Anschlüsse gefunden.", "%d port(s) found."},
    {"Erst trennen, der Anschluss ist belegt.", "Disconnect first, the port is in use."},
    {"Suche Anschluss, höre an jedem kurz mit…", "Searching port, listening briefly on each…"},
    {"Kein Gerät hat geantwortet. Der Anschluss ist nur nach USB-Kennung geraten.",
     "No device answered. The port was only guessed from its USB ID."},
    {"Baudrate muss eine Zahl sein.", "Baud rate must be a number."},
    {"Suche die SAROO…", "Searching for the SAROO…"},
    {"gültige Frames empfangen", "valid frames received"},
    {"SAROO-Klartext empfangen", "SAROO plain text received"},
    {"Bytes empfangen, nicht zuordenbar", "bytes received, not recognisable"},
    {"Vermutung, nichts gehört", "guess, nothing heard"},
    {"still", "silent"},
    {"Vermutung, nichts gehört. Wahrscheinlichster Anschluss nach USB-Kennung.",
     "Guess, nothing heard. Most likely port by USB ID."},
    {"Anschluss %s nicht zu öffnen (Fehler %lu)", "Cannot open port %s (error %lu)"},
    {"Baudrate oder Rahmenformat nicht setzbar", "Cannot set baud rate or frame format"},

    // --- Verbindung -------------------------------------------------------
    {"Kein Anschluss gewählt.", "No port selected."},
    {"Hook liefert Daten: %d Einträge im ersten Bericht.",
     "Hook is delivering data: %d entries in the first report."},
    {"Verbunden mit %s @ %d", "Connected to %s @ %d"},
    {"jedes Bild  \xC2\xB7  Erfassung %u us, höchstens %u us  \xC2\xB7  %ld Lücken",
     "every frame  \xC2\xB7  capture %u us, at most %u us  \xC2\xB7  %ld gaps"},
    {"jedes Bild  \xC2\xB7  %ld Lücken", "every frame  \xC2\xB7  %ld gaps"},
    {"Stichprobe alle 200 ms (kein Änderungsprotokoll)",
     "sample every 200 ms (no change log)"},
    {"Anschluss offen, warte auf das Gerät.", "Port open, waiting for the device."},
    {"Serieller Fehler, Verbindung verloren.", "Serial error, connection lost."},
    {"HELLO, Protokollversion %u, Spiel %s", "HELLO, protocol version %u, game %s"},
    {"SAROO hat den Hook-Satz angenommen. Er kommt auf die Karte und ist ab dem nächsten "
     "Start dieses Spiels aktiv.",
     "The SAROO accepted the hook set. It goes to the card and takes effect from the next "
     "start of this game."},
    {"zu groß für den Gerätepuffer", "too large for the device buffer"},
    {"Längen stimmen nicht überein", "lengths do not match"},
    {"Prüfsumme stimmt nicht", "checksum mismatch"},
    {"unbekannter Grund", "unknown reason"},
    {"SAROO hat den Hook-Satz abgelehnt: ", "The SAROO rejected the hook set: "},
    {"SAROO lehnt den Hook-Satz ab: zu groß für den Gerätepuffer.",
     "The SAROO rejects the hook set: too large for the device buffer."},
    {"Hook-Satz ist zu groß zum Übertragen.", "Hook set is too large to transfer."},
    {"Hook-Satz konnte nicht gesendet werden.", "Could not send the hook set."},
    {"Hook-Satz an die SAROO gesendet: %d Bytes in %d Teilen.",
     "Hook set sent to the SAROO: %d bytes in %d parts."},
    {"SAROO meldet sich und das Spiel ist erkannt, aber es kommen keine Watch-Werte: für "
     "dieses Spiel ist kein Hook aktiv. Entweder hat es kein Achievement-Set, oder das Spiel "
     "wurde seit dem Einrichten nicht neu gestartet — die M_-Zeilen wirken erst beim "
     "nächsten Spielstart.",
     "The SAROO responds and the game is detected, but no watch values arrive: no hook is "
     "active for this game. Either it has no achievement set, or the game has not been "
     "restarted since setup — the M_ lines only take effect on the next game start."},
    {"SAROO erkannt, sie meldet sich. Es kommen aber keine Watch-Werte: für das laufende "
     "Spiel ist kein Hook installiert. Dafür unter „Firmware und Spiele“ den "
     "ROM-Ordner einrichten.",
     "SAROO detected, it responds. But no watch values arrive: no hook is installed for "
     "the running game. Set up the ROM folder under “Firmware and games”."},
    {"Es kommen Daten an, aber sie sehen nicht nach SAROO aus. Stimmt der Anschluss und die "
     "Rate 1000000?",
     "Data is arriving, but it does not look like a SAROO. Are the port and the rate "
     "1000000 correct?"},
    {"Seit 12 Sekunden kommt nichts. Das kann normal sein, die SAROO meldet sich nur bei "
     "Plattenzugriffen. Bleibt es still: Anschluss, Rate 1000000, Verkabelung G an G, RXD "
     "an T, TXD an R, Konsole eingeschaltet prüfen.",
     "Nothing for 12 seconds. That can be normal, the SAROO only talks during disc access. "
     "If it stays silent, check: port, rate 1000000, wiring G to G, RXD to T, TXD to R, "
     "console switched on."},
    {"Hook liefert jetzt Watch-Werte.", "Hook is now delivering watch values."},
    {"Verbindung getrennt.", "Disconnected."},
    {"Verbindung verloren. Keine Angaben zum laufenden Spiel.",
     "Connection lost. No information about the running game."},
    {"Spiel erkannt: ", "Game detected: "},
    {"  Gebiet ", "  Region "},
    {"Prüfsumme aus dem Disc-Kopf gerechnet: ", "Checksum computed from the disc header: "},
    {"Frames kommen an, aber der Bildzähler steht seit über zehn Sekunden. Solange wird nicht "
     "ausgewertet, damit eingefrorene Werte nichts auslösen. Beim Laden ist das normal.",
     "Frames are arriving, but the frame counter has been stuck for more than ten seconds. "
     "Evaluation is paused until it moves again so that frozen values cannot trigger "
     "anything. During loading this is normal."},
    {"Hook läuft, Werte sind frisch.", "Hook running, values are fresh."},

    // --- RetroAchievements-Sitzung ----------------------------------------
    {"Hook konnte nicht erzeugt werden: ", "Could not create the hook: "},
    {"Hardcore: Speicherstände werden gesperrt. Das wirkt ab dem nächsten Start des Spiels.",
     "Hardcore: save states are being locked. This takes effect the next time the game "
     "starts."},
    {"Speicherstände werden freigegeben. Das wirkt ab dem nächsten Start des Spiels.",
     "Save states are being unlocked. This takes effect the next time the game starts."},
    {"Hardcore aktiv, Speicherstände gesperrt", "Hardcore active, save states locked"},
    {"Goldblitz am Fernseher", "Gold flash on the TV"},
    {"CRC-Fehler, Rohbytes: ", "CRC error, raw bytes: "},
    {"Bericht mit %d statt %d Einträgen, Bildzähler %08X, erste Adresse %08X.",
     "Report with %d instead of %d entries, frame counter %08X, first address %08X."},
    {"bei Freischaltungen", "on unlocks"},
    {"Goldblitz testen", "Test gold flash"},
    // Dateien auf die SD-Karte der SAROO. Die beiden Knopftexte gehen über
    // eine Variable an T().
    {"Dateien auf die SAROO-Karte", "Files to the SAROO card"},
    {"Gerät neu gestartet. Der laufende Frame ist dabei abgerissen, das ist "
     "kein Fehler.",
     "Device restarted. The frame in flight was cut off, that is not an error."},
    // Welcher Firmware-Stand laeuft wirklich. Das Trennzeichen ist ein
    // Mittelpunkt in UTF-8.
    {"STM32 %s  \xC2\xB7  RA noch nicht gehört", "STM32 %s  \xC2\xB7  RA not heard yet"},
    {"Übertrage %s nach %s auf der Karte: %d Bytes.",
     "Sending %s to %s on the card: %d bytes."},
    {"STM32 %s  \xC2\xB7  RA %s", "STM32 %s  \xC2\xB7  RA %s"},
    {"ssfirm %ld Byte", "ssfirm %ld bytes"},
    {"Saturn-Firmware (ssfirm.bin) übertragen",
     "Send Saturn firmware (ssfirm.bin)"},
    {"STM32-Firmware (mcuapp.bin) übertragen",
     "Send STM32 firmware (mcuapp.bin)"},
    {"Datei für die SAROO-Karte", "File for the SAROO card"},
    {"Übertragung abbrechen", "Cancel transfer"},
    {"Erst mit der SAROO verbinden.", "Connect to the SAROO first."},
    {"Wirkt beim nächsten Einschalten. Das Gerät sollte dabei im Startmenü "
     "stehen, nicht im Spiel.",
     "Takes effect at the next power-on. The device should sit in the start "
     "menu, not in a game."},
    {"Keine Verbindung zum Gerät.", "No connection to the device."},
    {"Es läuft schon eine Übertragung.", "A transfer is already running."},
    {"Datei lässt sich nicht öffnen: ", "Cannot open file: "},
    {"Die Datei ist leer.", "The file is empty."},
    {"%s liegt auf der Karte: %d Bytes in %lu Sekunden. Wirkt beim nächsten "
     "Einschalten.",
     "%s is on the card: %d bytes in %lu seconds. Takes effect at the next "
     "power-on."},
    {"SAROO hat die Übertragung abgelehnt: ",
     "The SAROO rejected the transfer: "},
    {"Die Datei auf der Karte ist unverändert.",
     "The file on the card is unchanged."},
    {"Übertragung abgebrochen. Die Datei auf der Karte ist unverändert.",
     "Transfer cancelled. The file on the card is unchanged."},
    {"Übertragung abgebrochen: Das Gerät hat nicht geantwortet. Die Datei auf "
     "der Karte ist unverändert.",
     "Transfer aborted: the device did not answer. The file on the card is "
     "unchanged."},
    {"Übertragung abgebrochen: Der Anschluss nimmt nichts mehr an.",
     "Transfer aborted: the port no longer accepts data."},
    {"Goldblitz: erst mit der SAROO verbinden.", "Gold flash: connect to the SAROO first."},
    {"Softcore, bis das Spiel mit gesperrten Speicherständen neu startet",
     "Softcore until the game restarts with save states locked"},
    {"Softcore: seit dem Spielstart wurde ein Speicherstand geladen",
     "Softcore: a save state was loaded since the game started"},
    {"Hardcore ist eingeschaltet, zählt aber erst, wenn die Konsole gesperrte "
     "Speicherstände meldet. Bis dahin werden Freischaltungen als Softcore gemeldet. "
     "Spiel einmal neu starten.",
     "Hardcore is enabled but only counts once the console reports locked save states. "
     "Until then, unlocks are submitted as softcore. Restart the game once."},
    {"Seit dem Spielstart wurde ein Speicherstand geladen. Freischaltungen werden bis "
     "zum nächsten Spielstart als Softcore gemeldet.",
     "A save state was loaded since the game started. Unlocks are submitted as softcore "
     "until the game is restarted."},
    {"Hardcore wirksam: Die Konsole meldet gesperrte Speicherstände.",
     "Hardcore in effect: the console reports locked save states."},
    {"Hook für %s wird an die SAROO geschickt: %d Watch-Adressen, %d Zeilen. Sie legt ihn "
     "selbst auf ihrer Karte ab.",
     "Sending hook for %s to the SAROO: %d watch addresses, %d lines. It stores it on its "
     "own card."},
    {"Hook könnte jetzt nachgetragen werden, aber der Pfad zur saroocfg.txt ist nicht "
     "hinterlegt. Einmal über „Firmware und Spiele“ die Datei auswählen, danach geht es "
     "von selbst.",
     "The hook could be added now, but no path to saroocfg.txt is set. Select the file "
     "once via “Firmware and games”, after that it works by itself."},
    {"Hook für %s ist fertig, aber noch nicht geschrieben. %s Er wird automatisch "
     "eingetragen, sobald die Karte am PC steckt.",
     "Hook for %s is ready but not written yet. %s It will be written automatically as "
     "soon as the card is connected to the PC."},
    {"saroocfg.txt nicht geschrieben: ", "saroocfg.txt not written: "},
    {" Der Hook bleibt vorgemerkt.", " The hook stays pending."},
    {"Hook für %s nachgetragen: %d Watch-Adressen in %s. Er wirkt beim NÄCHSTEN Start dieses "
     "Spiels, weil die SAROO die Zeilen nur beim Laden ausführt. Also einmal zurück ins Menü "
     "und das Spiel neu starten.",
     "Hook for %s added: %d watch addresses in %s. It takes effect on the NEXT start of "
     "this game, because the SAROO only applies the lines while loading. So return to the "
     "menu once and start the game again."},
    {"%d Hook(s) warten aufs Schreiben. %s", "%d hook(s) waiting to be written. %s"},
    {"saroocfg.txt weiterhin nicht geschrieben: ", "saroocfg.txt still not written: "},
    {"%d vorgemerkte(r) Hook(s) jetzt eingetragen: %s. Sicherung: %s. Karte zurück in die "
     "SAROO, dann läuft der Hook ab dem nächsten Start dieser Spiele.",
     "%d pending hook(s) now written: %s. Backup: %s. Put the card back into the SAROO; "
     "the hook runs from the next start of these games."},
    {"Benutzername und Passwort eingeben.", "Enter username and password."},
    {"Melde an…", "Logging in…"},
    {"Angemeldet als ", "Logged in as "},
    {"Anmeldung fehlgeschlagen: ", "Login failed: "},
    {"Abgemeldet.", "Logged out."},
    {"Spiel läuft: ", "Game running: "},
    {"Zum Auswerten erst bei RetroAchievements anmelden. Prüfsumme steht bereit: ",
     "Log in to RetroAchievements to evaluate. Checksum is ready: "},
    {"Frage RetroAchievements nach Prüfsumme ", "Asking RetroAchievements for checksum "},
    {"Nachfrage bei RetroAchievements fehlgeschlagen: ",
     "Request to RetroAchievements failed: "},
    {"RetroAchievements kennt diesen Abzug nicht (Prüfsumme %s). Live-Werte laufen weiter, "
     "es gibt nur nichts auszuwerten.",
     "RetroAchievements does not know this dump (checksum %s). Live values keep running, "
     "there is just nothing to evaluate."},
    {"Achievement-Set nicht ladbar: ", "Could not load the achievement set: "},
    {"Sitzung nicht gestartet: ", "Session not started: "},
    {"%s geladen: %d Achievements, %d bereits freigeschaltet.",
     "%s loaded: %d achievements, %d already unlocked."},
    {"RetroAchievements liefert %d Achievement(s), aber alle sind inoffiziell und zählen "
     "nicht. Deshalb gibt es nichts auszuwerten und keine Watch-Adressen zu beobachten.",
     "RetroAchievements returns %d achievement(s), but all of them are unofficial and do "
     "not count. So there is nothing to evaluate and no watch addresses to observe."},
    {"Für dieses Spiel gibt es bei RetroAchievements noch keine Achievements. Erkennung und "
     "Live-Werte laufen trotzdem, es gibt nur nichts auszuwerten.",
     "There are no achievements for this game on RetroAchievements yet. Detection and live "
     "values still work, there is just nothing to evaluate."},
    {"%d Achievement(s) nutzen Merkmale, die hier nicht abbildbar sind, und werden nie "
     "feuern.",
     "%d achievement(s) use features that cannot be mapped here and will never trigger."},
    {"Freigeschaltet: %s, %d Punkte", "Unlocked: %s, %d points"},
    {"Freischaltung nicht gemeldet (", "Unlock not reported ("},

    // --- Einrichtungsfenster ----------------------------------------------
    {"Bitte einen ROM-Ordner wählen.", "Please choose a ROM folder."},
    {"Suche Disc-Abbilder…", "Searching disc images…"},
    {"Keine Disc-Abbilder in diesem Ordner gefunden.", "No disc images found in this folder."},
    {"Fertig. %d von %d Spielen bekommen einen Hook.", "Done. %d of %d games get a hook."},
    {"SAROO einrichten: %d Spiel(e) bereit.", "Set up SAROO: %d game(s) ready."},
    {"Bitte die saroocfg.txt wählen.", "Please choose the saroocfg.txt."},
    {"Geschrieben. %d Spiel(e) eingetragen.", "Written. %d game(s) added."},
    {"saroocfg.txt geschrieben, Sicherung: ", "saroocfg.txt written, backup: "},
    {"%d neue Kennung(en) in der Nachschlagetabelle.", "%d new ID(s) in the lookup table."},
    {"Fehler: ", "Error: "},
    {"Schreiben fehlgeschlagen: ", "Writing failed: "},
    {"ROM-Ordner der SD-Karte", "ROM folder on the SD card"},
    {"Ordner wählen…", "Choose folder…"},
    {"ROM-Ordner wählen", "Choose ROM folder"},
    {"Pfad zur saroocfg.txt", "Path to saroocfg.txt"},
    {"Datei wählen…", "Choose file…"},
    {"saroocfg.txt wählen", "Choose saroocfg.txt"},
    {"Unterordner mit durchsuchen", "Include subfolders"},
    {"Nur Spiele mit Achievement-Set einrichten", "Only set up games with an achievement set"},
    {"Aus: jedes erkannte Saturn-Abbild wird eingerichtet.\nDer Hook hängt nicht am Set, und "
     "nur so meldet jedes\nlaufende Spiel im Betrieb selbst seine Kennung.",
     "Off: every detected Saturn image is set up.\nThe hook does not depend on the set, and "
     "only this way does every\nrunning game report its ID by itself."},
    {"Prüfen", "Check"},
    {"Schreiben", "Write"},
    {"Erst die saroocfg.txt oben auswählen.", "Select the saroocfg.txt above first."},
    {"Bestehende Einträge bleiben erhalten. Vorher wird gesichert.",
     "Existing entries are kept. A backup is made first."},
    {"Kennung", "ID"},
    {"%d (+%d inoff.)", "%d (+%d unoff.)"},
    {"Nichts weiter anzumerken.", "Nothing else to note."},
    {"Zeile anklicken, um die Begründung zu sehen.", "Click a row to see the reason."},

    // --- Einrichtung, Kernschicht -----------------------------------------
    {"Japan", "Japan"},
    {"Asien", "Asia"},
    {"USA", "USA"},
    {"Europa", "Europe"},
    {"Brasilien", "Brazil"},
    {"Korea", "Korea"},
    {"Asien (NTSC)", "Asia (NTSC)"},
    {"Lateinamerika", "Latin America"},
    {"Die in der .cue genannte Datei wurde nicht gefunden.",
     "The file named in the .cue was not found."},
    {"Datei nicht lesbar.", "File not readable."},
    {"Kein gültiger Saturn-Kopf gefunden. Falsches Abbild, falsche Spur, oder ein "
     "unübliches Sektorformat.",
     "No valid Saturn header found. Wrong image, wrong track, or an unusual sector format."},
    {"zu viele Adressen", "too many addresses"},
    {"Config-Puffer zu klein", "config buffer too small"},
    {"%d Paare, der Puffer fasst etwa %d.", "%d pairs, the buffer holds about %d."},
    {"bereit", "ready"},
    {"Messung", "Measurement"},
    {"normal (nur Daten)", "normal (data only)"},
    {"alter Satz mit Hook-Code", "old set with hook code"},
    {"Hook-Satz ohne Vektorzeile", "Hook set without the vector line"},
    {"Nur zur Fehlersuche. Außer bei der ersten Stufe liefert der Hook keine oder "
     "unvollständige Werte.",
     "For diagnostics only. Except for the first setting the hook delivers no or "
     "incomplete values."},
    {"nur zuhören, nichts senden", "listen only, send nothing"},
    {"Kein Hook-Satz, keine Dateiübertragung.",
     "No hook set, no file transfer."},
    {"Mailbox an anderer Adresse", "Mailbox at a different address"},
    {"Schreibt den Satz nach 0x02380000 statt 0x023F0000. Der Hook liefert dann keine Werte.",
     "Writes the set to 0x02380000 instead of 0x023F0000. The hook delivers no values then."},
    {"%d/%d · %d %% · noch etwa %d:%02d min", "%d/%d · %d %% · about %d:%02d min left"},
    {"%d/%d · %d %% · noch etwa %d s", "%d/%d · %d %% · about %d s left"},
    {"%d Achievement(s) nutzen nicht abbildbare Merkmale und werden nie feuern.",
     "%d achievement(s) use features that cannot be mapped and will never trigger."},
    {"bereit, ohne Achievements", "ready, no achievements"},
    {"kein Saturn-Abbild", "not a Saturn image"},
    {"kein Achievement-Set", "no achievement set"},
    {"nur inoffizielles Set", "unofficial set only"},
    {"%s: RetroAchievements liefert %d Achievement(s), aber alle sind inoffiziell und zählen "
     "nicht. Der Hook wird trotzdem installiert.",
     "%s: RetroAchievements returns %d achievement(s), but all of them are unofficial and do "
     "not count. The hook is installed anyway."},
    {"%s ist bei RetroAchievements bekannt, hat aber kein Set. Der Hook wird trotzdem "
     "installiert.",
     "%s is known to RetroAchievements but has no set. The hook is installed anyway."},
    {"bereit, RA-Hash unbekannt", "ready, RA hash unknown"},
    {"%s — Titel und Kennung stammen aus dem Disc-Kopf (Gebiet %s). RetroAchievements kennt "
     "genau diesen Abzug nicht, Prüfsumme %s. Ein anderer Abzug desselben Spiels kann "
     "funktionieren. Der Hook wird trotzdem installiert.",
     "%s — title and ID come from the disc header (region %s). RetroAchievements does not "
     "know this exact dump, checksum %s. A different dump of the same game may work. The "
     "hook is installed anyway."},
    {"unbekannt", "unknown"},
    {"Es ist kein Pfad zur saroocfg.txt hinterlegt.", "No path to saroocfg.txt is set."},
    {"%s ist da, lässt sich aber nicht öffnen. Schreibschutz am Kartenleser oder die Datei "
     "ist gerade in Benutzung?",
     "%s exists but cannot be opened. Write protection on the card reader, or is the file "
     "in use?"},
    {"In %s gibt es diese Datei nicht: %s", "This file does not exist in %s: %s"},
    {"dem Ordner", "the folder"},
    {"Laufwerk %s ist nicht da. Die SD-Karte steckt vermutlich in der SAROO — während des "
     "Spielens ist das normal.",
     "Drive %s is not available. The SD card is probably in the SAROO — that is normal "
     "while playing."},
    {"Zugriff auf %s verweigert.", "Access to %s denied."},
    {"%s ist nicht erreichbar (Windows-Fehler %lu).", "%s is not reachable (Windows error %lu)."},
    {"Sicherung konnte nicht angelegt werden, es wurde nichts geschrieben.",
     "Could not create a backup, nothing was written."},
    {"saroocfg.txt nicht beschreibbar.", "saroocfg.txt is not writable."},
    {"%d Watch-Adressen, aber ra_mailbox.c akzeptiert nur %d. Dort zuerst erhöhen und den "
     "Patch neu bauen, sonst schneidet die Firmware überzählige Einträge ohne Fehlermeldung "
     "ab.",
     "%d watch addresses, but ra_mailbox.c only accepts %d. Raise it there first and rebuild "
     "the patch, otherwise the firmware silently drops the extra entries."},
    {"%d Watch-Adressen brauchen mehr Platz als bis zum Hook-Code bei +0x%x verfügbar ist "
     "(höchstens %d).",
     "%d watch addresses need more space than is available up to the hook code at +0x%x "
     "(at most %d)."},

    // --- Netz und RetroAchievements ---------------------------------------
    {"WinHttpOpen fehlgeschlagen", "WinHttpOpen failed"},
    {"Verbindung zu %s fehlgeschlagen", "Connection to %s failed"},
    {"Anfrage konnte nicht angelegt werden", "Could not create the request"},
    {"Senden fehlgeschlagen (Fehler %lu)", "Sending failed (error %lu)"},
    {"Keine Antwort (Fehler %lu)", "No response (error %lu)"},
    {"Ungültige Server-Adresse.", "Invalid server address."},
    {"Keine Antwort vom Server.", "No response from the server."},
    {"Anfragelimit erreicht, später erneut versuchen.", "Request limit reached, try again later."},
    {"Der Server hat die Anfrage abgelehnt.", "The server rejected the request."},
    {"Anmeldung ohne Zugangsmarke, das sollte nicht vorkommen.",
     "Login returned no token, this should not happen."},
};

const std::unordered_map<std::string, const char*>& Tabelle() {
    // Einmal aufgebaut, danach nur gelesen -- aus jedem Thread sicher.
    static const std::unordered_map<std::string, const char*> t = [] {
        std::unordered_map<std::string, const char*> m;
        for (const Paar& p : kTabelle) m.emplace(p.de, p.en);
        return m;
    }();
    return t;
}

}  // namespace

void SpracheSetzen(Sprache s) { gSprache = (int)s; }
Sprache AktuelleSprache()     { return (Sprache)gSprache.load(); }

const char* Kennung(Sprache s) { return s == Sprache::En ? "en" : "de"; }

Sprache AusKennung(const char* kennung) {
    return (kennung && std::strcmp(kennung, "en") == 0) ? Sprache::En : Sprache::De;
}

const char* T(const char* de) {
    if (!de || gSprache.load() != (int)Sprache::En) return de;
    const auto& t = Tabelle();
    const auto it = t.find(de);
    return it == t.end() ? de : it->second;
}

}  // namespace i18n
