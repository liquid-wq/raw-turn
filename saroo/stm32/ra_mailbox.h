/* ra_mailbox.h -- RAW-TURN auf der STM32, siehe ra_mailbox.c. */
#ifndef RA_MAILBOX_H
#define RA_MAILBOX_H

/* Startet den Thread, der Werte und Spielerkennung an den PC meldet.
 * Einmalig aufzurufen, am Ende von saturn_config() (saturn_main.c). */
void ra_mailbox_start(void);

/* Uebergibt die 512 Byte Disc-Kopf des gerade geladenen Abbilds. Der PC
 * rechnet daraus Titel und RetroAchievements-Pruefsumme. NULL loescht die
 * Meldung (kein Abbild geladen). Aufruf aus cdimg.c, get_gameid(). */
void ra_mailbox_set_gameinfo(const void *ip512);

/* Aus dem UART4-Empfangsinterrupt fuer jedes eintreffende Byte aufzurufen.
 * Rueckgabe 1: Das Byte gehoerte zu einem RAW-TURN-Rahmen und ist verbraucht.
 * Rueckgabe 0: Der Aufrufer reicht es an die Shell weiter. */
int ra_uart_rx_byte(unsigned char b);

/* Nur aus dem Disc-Thread aufzurufen: FatFs ist in diesem Build nicht
 * threadsicher (FF_FS_REENTRANT 0).
 *
 * ra_hookfile_flush(): schreibt einen vom PC empfangenen Hook-Satz auf die
 * Karte. Tut nichts, wenn nichts wartet.
 *
 * ra_hookfile_apply(): liest den Hook-Satz zum laufenden Spiel und haengt
 * seine M_-Zeilen an den Konfigurationspuffer an. NACH parse_config()
 * aufrufen, denn das setzt den Puffer zurueck. */
void ra_hookfile_flush(void);
void ra_hookfile_apply(const char *gameid);

/* Schreibt eine vom PC uebertragene Datei blockweise auf die Karte und
 * quittiert jeden Block. Aufruf aus dem Leerlaufzweig von disk_task()
 * (saturn_main.c). Tut nichts, wenn nichts wartet. */
void ra_filexfer_tick(void);

#endif
