// Einblendung bei freigeschaltetem Achievement.
//
// Eine Karte schiebt sich unten rechts auf dem Bildschirm herein, federt kurz
// nach, laesst einen Glanz darueber laufen, bleibt stehen und verschwindet.
// Sie liegt ueber allen Fenstern und nimmt keine Eingaben an, ist also auch
// sichtbar, wenn das Werkzeug im Hintergrund laeuft. Ablauf und Gestaltung
// folgen der Einblendung in MEGA-RAW.
//
// Gezeichnet wird mit GDI+ in ein geschichtetes Win32-Fenster. GDI+ gehoert zu
// Windows und dekodiert auch die PNG-Badges, das Programm bleibt damit eine
// einzelne Datei ohne Bildbibliothek.
#pragma once

#include "app_state.h"

namespace popup {

// Zeigt die Einblendung. Laeuft gerade eine, wird die neue danach gezeigt.
// Spielt dazu den Freischalt-Ton.
void Zeigen(const app::Freischaltung& f, bool englisch, float skalierung);

// Beim Programmende aufrufen.
void Beenden();

}  // namespace popup
