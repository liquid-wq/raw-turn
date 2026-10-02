// Sprachen: Deutsch und Englisch.
//
// Muster wie in mega-raw (i18n.h): Der deutsche Text steht im Quelltext und ist
// zugleich der Schluessel. T() liefert bei Englisch die Uebersetzung aus der
// Tabelle in i18n.cpp, sonst den deutschen Text unveraendert. Fehlt eine
// Uebersetzung, erscheint Deutsch statt einer Luecke.
//
// Liegt in core/, weil auch Meldungen der Kernschicht (Fehlertexte beim
// Schreiben der saroocfg.txt, Anschlusssuche) im Fenster landen. Es haengt an
// keiner Oberflaechen-Bibliothek.
#pragma once

namespace i18n {

enum class Sprache { De, En };

void SpracheSetzen(Sprache s);
Sprache AktuelleSprache();
const char* Kennung(Sprache s);           // "de" / "en", fuer die Einstellungsdatei
Sprache AusKennung(const char* kennung);  // Unbekanntes ergibt Deutsch

// Deutscher Text rein, Text in der aktuellen Sprache raus. Der Zeiger bleibt
// gueltig, solange das Programm laeuft. Aus jedem Thread aufrufbar.
const char* T(const char* de);

}  // namespace i18n

// Markiert einen deutschen Text als Uebersetzungsschluessel, der NICHT sofort,
// sondern erst bei der Anzeige mit T() uebersetzt wird -- etwa Zustandshinweise,
// die sonst bei einem Sprachwechsel in der alten Sprache stehen blieben. Tut zur
// Laufzeit nichts.
#define TK(de) (de)

using i18n::T;
