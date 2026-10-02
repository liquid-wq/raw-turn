/* ---------------------------------------------------------------------------
 * ra_mailbox.c -- RAW-TURN auf der STM32 der SAROO.
 *
 * Aufgaben:
 *   - die Watch-Werte der Saturn an den PC melden: das Aenderungsprotokoll
 *     (jedes Bild) und die Mailbox (Adressliste, Rundenzaehler), beide im
 *     SDRAM, als Rahmen ueber UART4;
 *   - den Disc-Kopf des geladenen Spiels melden, damit der PC das Spiel
 *     erkennt (GAME_INFO);
 *   - Rahmen vom PC annehmen: Hook-Saetze, Dateien fuer die SD-Karte,
 *     Speicherstand-Auftraege und die Uhrzeit.
 *
 * Laeuft als eigener, niedrig priorisierter RTOS-Thread (osPriorityLow, wie
 * main_task) und stoert die CD-Emulation nicht (sirq_task laeuft mit
 * osPriorityNormal+7, disk_task mit osPriorityHigh).
 *
 * Benutzt uart4_put_buf() (dafuer in uart4.c oeffentlich gemacht), FatFs,
 * ss_config_put() und die CMSIS-RTOS2-API.
 *
 * Die Saturn schreibt big-endian, die STM32 liest little-endian, daher
 * ra_bswap32 beim Lesen aus dem SDRAM.
 * --------------------------------------------------------------------------- */

#include <string.h>
#include <stdlib.h>
#include "main.h"
#include "ff.h"
#include "ra_mailbox.h"

/* In saturn_utils.c definiert. Legt ein Adress-/Wert-Paar im
 * Konfigurationspuffer ab, den game_patch() auf der Saturn-Seite vor dem
 * Spielstart abarbeitet. */
extern void ss_config_put(u32 val);

/* Mailbox-Basis aus Sicht der STM32 (Saturn: 0x223F0000). */
#define RA_MBOX_BASE    0x613f0000u

#define RA_MAX_ENTRIES  300     /* wie RA_MAX_WATCH in ra_ingame.c und
                                 * kFirmwareMaxEintraege im Werkzeug */
#define RA_FRAME_MAX    250     /* LEN-Grenze eines Rahmens */
#define RA_ENTRY_BYTES  9       /* ADDR(4)+SIZE(1)+VALUE(4); SIZE immer 4 */
#define RA_SEQLOCK_RETRIES 300  /* siehe ra_spin_delay() */
#define RA_POLL_MS      20      /* Takt der Schleife: Quittungen, Spielkennung,
                                 * Aenderungsprotokoll */

/* Die Mailbox nur in jeder zehnten Runde lesen, also alle 200 ms. Das Lesen
 * greift auf dasselbe SDRAM zu wie die Saturn ueber das FPGA; haeufigeres
 * Lesen kann Spiele stoeren. Jedes Bild kommt ohnehin ueber das
 * Aenderungsprotokoll. */
#define RA_LESE_TEILER  10

/* "RASA" bei +0x00: Diese Kennung schreibt der Hook-Satz des Spiels mit. Ohne
 * Hook-Satz steht dort uninitialisiertes SDRAM, das diesen Wert nicht durch
 * Zufall traegt; dann wird nichts gesendet. */
#define RA_MAGIC 0x52415341u

/* Nach so vielen Lesedurchgaengen mit unveraendertem Rundenzaehler wird nur
 * noch selten gesendet. Deckt den Fall ab, dass ein Hook lief, jetzt aber
 * steht (Spiel gewechselt ohne Ausschalten, die Kennung bleibt im SDRAM). Der
 * PC erkennt den Stillstand selbst und bekommt trotzdem gelegentlich ein
 * Lebenszeichen. */
#define RA_STALE_PING_POLLS 10   /* Lesedurchgaenge zu je 200 ms */

#define FRAME_MAGIC1       0xA5
#define FRAME_MAGIC2       0x5A
#define TYPE_WATCH_REPORT  0x01
#define TYPE_GAME_INFO     0x04

/* Spielerkennung: die 512 Byte Disc-Kopf, die die Firmware beim Laden des
 * Abbilds ohnehin von der SD-Karte liest. Daraus rechnet der PC Titel und
 * RetroAchievements-Pruefsumme. Unabhaengig von Hook und Konfiguration.
 *
 * LEN im Drahtformat ist ein Byte, also maximal 250 Nutzbytes je Rahmen. Der
 * Kopf geht deshalb als vier Teile mit vorangestelltem Offset hinaus. */
#define RA_IP_LEN        512
#define RA_GI_CHUNK      128
#define RA_GI_RESEND_POLLS 100   /* bei RA_POLL_MS=20 etwa alle 2 s */

static u8 ra_gameinfo[RA_IP_LEN];
static volatile int ra_gameinfo_valid = 0;
/* Steigt mit jedem neuen Spiel. Der Sende-Thread vergleicht dagegen und
 * schickt den Kopf dann sofort, ohne auf das Wiederholintervall zu warten. */
static volatile u32 ra_gameinfo_gen = 0;

static u8 ra_seq = 0;

static u32 ra_bswap32(u32 v)
{
	return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) |
	       ((v & 0x00ff0000u) >> 8)  | ((v & 0xff000000u) >> 24);
}

/* CRC-8, Polynom 0x07, Startwert 0, kein finales XOR -- wie Crc8() im
 * Werkzeug (core/protocol.cpp). Fortsetzbar, damit der Empfang die Nutzdaten
 * nicht umkopieren muss. */
static u8 ra_crc8_cont(u8 crc, const u8 *data, int len)
{
	int i, b;
	for (i = 0; i < len; i++) {
		crc ^= data[i];
		for (b = 0; b < 8; b++) {
			if (crc & 0x80) crc = (u8)((crc << 1) ^ 0x07);
			else             crc = (u8)(crc << 1);
		}
	}
	return crc;
}

static u8 ra_crc8(const u8 *data, int len)
{
	return ra_crc8_cont(0x00, data, len);
}

/* In Main/uart4.c, dafuer dort von static auf oeffentlich umgestellt. */
extern int uart4_put_buf(void *data, int length);
extern u32 uart4_tc_frueh;

/* Ueber denselben gepufferten Weg wie printk() senden. uart4_put_buf() nimmt
 * einen Rahmen ganz oder gar nicht in den Ringpuffer, ein Rahmen wird also
 * nie mit Textausgaben vermischt. Ist gerade kein Platz, kurz warten; nach
 * rund 200 ms wird der Rahmen verworfen statt den Thread festzuhalten. */
static void ra_uart_send(const u8 *buf, int len)
{
	int versuche;
	for (versuche = 0; versuche < 200; versuche++) {
		if (uart4_put_buf((void*)buf, len) == 0) return;
		osDelay(1);
	}
}

typedef struct {
	u32 addr;
	u32 value;
} ra_entry_t;

/* Prueft die Kennung bei +0x00, siehe RA_MAGIC. */
static int ra_mailbox_valid(void)
{
	volatile u32 *mbox = (volatile u32 *)RA_MBOX_BASE;
	return ra_bswap32(mbox[0]) == RA_MAGIC;
}

/* Kurze Pause zwischen zwei Seqlock-Versuchen. Ohne sie koennten alle
 * Versuche schneller ablaufen als ein einziger Schreibzugriff der Saturn. */
static void ra_spin_delay(void)
{
	volatile int i;
	for (i = 0; i < 200; i++) { }
}

/* Liest einen konsistenten Stand per Seqlock: Rundenzaehler vor und nach dem Lesen der Eintraege vergleichen, beide
 * muessen gleich und gerade sein. Gibt die Anzahl gelesener Eintraege zurueck
 * (kann 0 sein), oder -1, wenn nach RA_SEQLOCK_RETRIES Versuchen kein
 * konsistenter Stand zustande kam; dann wird dieser Zyklus ausgelassen. */
static int ra_read_snapshot(u32 *frame_count, ra_entry_t *out, int max_entries)
{
	volatile u32 *mbox = (volatile u32 *)RA_MBOX_BASE;
	int retry;

	for (retry = 0; retry < RA_SEQLOCK_RETRIES; retry++) {
		u32 fc1, fc2, ec_raw, n, i;

		fc1 = ra_bswap32(mbox[2]);      /* +0x08 FRAME_COUNT */
		if (fc1 & 1) { ra_spin_delay(); continue; }  /* Schreibvorgang laeuft */

		ec_raw = ra_bswap32(mbox[3]);   /* +0x0c ENTRY_COUNT|RESERVED */
		n = ec_raw >> 16;
		if (n > (u32)max_entries) n = (u32)max_entries;

		for (i = 0; i < n; i++) {
			u32 base = 4 + i * 3;    /* +0x10 + i*12, in 32-Bit-Worten */
			out[i].addr  = ra_bswap32(mbox[base + 0]);
			out[i].value = ra_bswap32(mbox[base + 2]);
		}

		fc2 = ra_bswap32(mbox[2]);
		if (fc1 == fc2) {
			*frame_count = fc1;
			return (int)n;
		}
		ra_spin_delay();
	}
	return -1;
}

/* Sendet einen oder mehrere WATCH_REPORT-Rahmen fuer einen Stand. Mehrere nur,
 * wenn mehr Eintraege vorliegen, als ein Rahmen fasst (LEN<=250); alle tragen
 * denselben FRAME_COUNT. Bei count==0 geht genau ein Rahmen mit leerem
 * Eintragsteil hinaus, als Lebenszeichen. */
static void ra_send_watch_report(u32 frame_count, const ra_entry_t *entries, int count)
{
	u8 buf[6 + RA_FRAME_MAX];
	int sent = 0;
	const int max_chunk = (RA_FRAME_MAX - 4) / RA_ENTRY_BYTES;

	do {
		int chunk = count - sent;
		int i, len, p;

		if (chunk > max_chunk) chunk = max_chunk;
		if (chunk < 0) chunk = 0;

		len = 4 + chunk * RA_ENTRY_BYTES;

		buf[0] = FRAME_MAGIC1;
		buf[1] = FRAME_MAGIC2;
		buf[2] = TYPE_WATCH_REPORT;
		buf[3] = ra_seq++;
		buf[4] = (u8)len;

		p = 5;
		buf[p+0] = (u8)(frame_count >> 24);
		buf[p+1] = (u8)(frame_count >> 16);
		buf[p+2] = (u8)(frame_count >> 8);
		buf[p+3] = (u8)(frame_count);
		p += 4;

		for (i = 0; i < chunk; i++) {
			u32 addr = entries[sent + i].addr;
			u32 val  = entries[sent + i].value;
			buf[p+0] = (u8)(addr >> 24);
			buf[p+1] = (u8)(addr >> 16);
			buf[p+2] = (u8)(addr >> 8);
			buf[p+3] = (u8)(addr);
			buf[p+4] = 4;   /* SIZE */
			buf[p+5] = (u8)(val >> 24);
			buf[p+6] = (u8)(val >> 16);
			buf[p+7] = (u8)(val >> 8);
			buf[p+8] = (u8)(val);
			p += RA_ENTRY_BYTES;
		}

		buf[p] = ra_crc8(&buf[2], 3 + len);   /* CRC ueber TYPE,SEQ,LEN,PAYLOAD */

		ra_uart_send(buf, p + 1);

		sent += chunk;
	} while (sent < count);
}

/* Von cdimg.c aufgerufen, sobald ein Abbild geladen ist und der Disc-Kopf im
 * Lesepuffer steht. Kopiert nur, sendet nicht: Der Aufruf kommt aus dem
 * Platten-Thread, gesendet wird im eigenen. */
void ra_mailbox_set_gameinfo(const void *ip512)
{
	if(ip512==NULL){
		ra_gameinfo_valid = 0;
		return;
	}
	memcpy(ra_gameinfo, ip512, RA_IP_LEN);
	ra_gameinfo_valid = 1;
	ra_gameinfo_gen += 1;
}

/* Den Disc-Kopf als vier GAME_INFO-Rahmen senden.
 * Nutzdaten je Rahmen: OFFSET (2 Byte, big-endian) + RA_GI_CHUNK Datenbytes.
 *
 * Rueckgabe: die Generation, die vollstaendig gesendet wurde, oder 0, wenn
 * waehrend des Sendens ein anderes Spiel geladen wurde. Der Aufrufer merkt
 * sich nur ein vollstaendiges Ergebnis als gesendet; ein halb altes, halb
 * neues Kopfstueck ergaebe beim PC eine falsche Pruefsumme. */
static u32 ra_send_gameinfo(void)
{
	u8 buf[6 + 2 + RA_GI_CHUNK];
	int off;
	u32 gen = ra_gameinfo_gen;

	for(off = 0; off < RA_IP_LEN; off += RA_GI_CHUNK){
		int len = 2 + RA_GI_CHUNK;
		int p;

		buf[0] = FRAME_MAGIC1;
		buf[1] = FRAME_MAGIC2;
		buf[2] = TYPE_GAME_INFO;
		buf[3] = ra_seq++;
		buf[4] = (u8)len;

		p = 5;
		buf[p+0] = (u8)(off >> 8);
		buf[p+1] = (u8)(off);
		memcpy(&buf[p+2], ra_gameinfo + off, RA_GI_CHUNK);
		p += len;

		buf[p] = ra_crc8(&buf[2], 3 + len);
		ra_uart_send(buf, p + 1);
	}

	return (ra_gameinfo_gen==gen) ? gen : 0;
}

/* ===========================================================================
 * Hook-Saetze von der SD-Karte
 *
 * Eine Datei je Spiel, /SAROO/rahook_<Spielkennung>.txt, mit denselben
 * M_-Zeilen, die sonst in saroocfg.txt stuenden. Sie werden NACH
 * parse_config() an den Konfigurationspuffer angehaengt, gelten also
 * zusaetzlich zu allem, was in saroocfg.txt steht. saroocfg.txt selbst wird
 * nie angefasst. Geschrieben werden die Dateien von der SAROO selbst, weil die
 * Karte beim Spielen im Geraet steckt.
 * =========================================================================== */

/* Flacher Name statt eigenem Ordner: Ein Unterordner kostete f_mkdir(), und
 * der App-Bereich im ITCM ist knapp. */
#define RA_HOOK_PRE      "/SAROO/rahook_"
#define RA_HOOK_MAX      12288

static char ra_hookbuf[RA_HOOK_MAX];
static volatile int  ra_hook_len = 0;      /* gefuellte Bytes */
static volatile int  ra_hook_soll = 0;     /* angekuendigte Gesamtlaenge */
static volatile int  ra_hook_bereit = 0;   /* vollstaendig empfangen */
static char ra_hook_name[40];              /* Dateiname ohne Ordner */

/* Spielkennung in einen Dateinamen umsetzen: Leerzeichen zu '_', hinten
 * abgeschnitten. Aus "T-10001G  V1.000" wird "T-10001G__V1.000.txt". */
static void ra_hook_dateiname(const char *gameid, char *aus, int max)
{
	int i, n = 0;

	for(i=0; i<16 && gameid[i] && n<max-6; i++){
		char c = gameid[i];
		if(c==' ') c = '_';
		if(c=='/' || c=='\\' || c==':' || c=='*' || c=='?' ||
		   c=='"' || c=='<' || c=='>' || c=='|') c = '_';
		aus[n++] = c;
	}
	while(n>0 && aus[n-1]=='_') n -= 1;   /* nachlaufende Fueller weg */
	aus[n] = 0;
	strcat(aus, ".txt");
}

/* Eine Zeile "M_xxxxxxxx=xxxxxxxx" in den Konfigurationspuffer legen.
 * Kodierung wie config_wrmem() in saturn_utils.c: die Zahl der Wert-Ziffern
 * bestimmt die Zugriffsbreite und steht in den oberen vier Bit der Adresse. */
static int ra_hook_zeile(char *zeile)
{
	char *p;
	u32 addr, val;
	int breite;

	while(*zeile==' ' || *zeile=='\t') zeile++;
	if(zeile[0]!='M' || zeile[1]!='_') return 0;

	p = strchr(zeile, '=');
	if(p==NULL) return 0;
	*p = 0;

	addr = strtoul(zeile+2, NULL, 16);
	p += 1;
	while(*p==' ') p++;
	breite = strlen(p);
	if(breite!=2 && breite!=4 && breite!=8) return 0;
	val = strtoul(p, NULL, 16);

	ss_config_put(((u32)(breite/2)<<28) | (addr & 0x0fffffffu));
	ss_config_put(val);
	return 1;
}

void ra_hookfile_flush(void)
{
	FIL fp;
	char pfad[64];
	u32 geschrieben;

	if(!ra_hook_bereit) return;
	ra_hook_bereit = 0;

	strcpy(pfad, RA_HOOK_PRE);
	strcat(pfad, ra_hook_name);

	if(f_open(&fp, pfad, FA_WRITE|FA_CREATE_ALWAYS)){
		printk("ra: hook write failed\n");
		return;
	}
	f_write(&fp, ra_hookbuf, ra_hook_len, &geschrieben);
	f_sync(&fp);
	f_close(&fp);
	printk("ra: hook saved %s\n", pfad);
}

void ra_hookfile_apply(const char *gameid)
{
	FIL fp;
	char pfad[64], name[40];
	char happen[128];               /* stueckweise lesen, schont Stapel */
	char zeile[64];
	u32 gelesen;
	int zl = 0, eingetragen = 0;

	if(gameid==NULL) return;

	ra_hook_dateiname(gameid, name, sizeof(name));
	strcpy(pfad, RA_HOOK_PRE);
	strcat(pfad, name);

	if(f_open(&fp, pfad, FA_READ)) return;   /* kein Hook-Satz fuer dies Spiel */

	for(;;){
		int i;
		gelesen = 0;
		if(f_read(&fp, happen, sizeof(happen), &gelesen) || gelesen==0) break;
		for(i=0; i<(int)gelesen; i++){
			char c = happen[i];
			if(c=='\n' || c=='\r'){
				if(zl>0){
					zeile[zl] = 0;
					eingetragen += ra_hook_zeile(zeile);
					zl = 0;
				}
			}else if(zl < (int)sizeof(zeile)-1){
				zeile[zl++] = c;
			}
		}
	}
	if(zl>0){                       /* letzte Zeile ohne Zeilenende */
		zeile[zl] = 0;
		eingetragen += ra_hook_zeile(zeile);
	}
	f_close(&fp);
	/* Mit Baudatum: Die Zeile kommt bei jedem Spielstart, also auch dann beim
	 * PC-Werkzeug an, wenn es erst nach dem Einschalten verbindet. */
	printk("ra: hook %d pairs, mcu " __DATE__ "\n", eingetragen);
}

/* ===========================================================================
 * Dateien vom PC auf die Karte
 *
 * Damit lassen sich ssfirm.bin und mcuapp.bin aufspielen, ohne die Karte aus
 * der SAROO zu nehmen.
 *
 * Ablauf: Der PC kuendigt Pfad und Gesamtlaenge an, schickt die Nutzdaten in
 * Bloecken und zuletzt Laenge und Bytesumme. Quittiert wird nach dem Oeffnen,
 * nach jedem vollen Block und am Ende. Die Quittung ist zugleich die Bremse:
 * Der PC sendet erst weiter, wenn der Block auf der Karte steht.
 *
 * Geschrieben wird nicht im Interrupt und nicht in diesem Thread, sondern im
 * Leerlaufzweig von disk_task() (saturn_main.c). FatFs ist in diesem Build
 * nicht threadsicher (FF_FS_REENTRANT 0).
 *
 * Geschrieben wird immer nach <pfad>.$$$. Erst wenn Laenge und Bytesumme
 * stimmen, ersetzt die Zwischendatei das Ziel. Ein abgebrochener Transfer
 * kann deshalb keine vorhandene Firmware unbrauchbar machen. Jeder Fehler
 * bricht den ganzen Transfer ab; der PC faengt von vorn an.
 * =========================================================================== */

/* ===========================================================================
 * Empfang vom PC
 *
 * Laeuft im Interrupt und macht deshalb nur das Noetigste: Rahmen zerlegen,
 * Nutzdaten kopieren, Marke setzen. Gesendet wird im Thread, geschrieben im
 * Disc-Thread.
 *
 * Die Shell auf derselben Leitung bleibt bedienbar: Alles, was nicht zu einem
 * Rahmen gehoert, wird durchgereicht. Ein einzelnes 0xA5 ohne folgendes 0x5A
 * geht dabei verloren; in der ASCII-Eingabe einer Shell kommt es nicht vor.
 * =========================================================================== */

#define TYPE_HOOK_BEGIN  0x10
#define TYPE_HOOK_DATA   0x11
#define TYPE_HOOK_END    0x12
/* Quittung fuer alles, was der PC schickt: welcher Rahmen, und wie es ausging. */
#define TYPE_HOOK_ACK    0x13
/* Beliebige Datei auf die Karte, siehe oben. */
#define TYPE_FILE_BEGIN  0x15
#define TYPE_FILE_DATA   0x16
#define TYPE_FILE_END    0x17
/* Speicherstaende. PC -> hier: 2 Byte (Vorgang, Slot), abgelegt als
 * Auftragswort bei Mailbox +0xFE0. Hier -> PC: das Antwortwort der Saturn von
 * +0xFE4 (4 Byte) und die Slot-Uebersicht, sobald sich die Antwort aendert.
 * Gegenstueck: RA_AUFTRAG_WORT in ra_ingame.c. */
#define TYPE_STAND       0x18
#define TYPE_STAND_ANTW  0x19
#define RA_AUFTRAG_OFS   0x0fe0
#define RA_ANTWORT_OFS   0x0fe4
/* 14 Slots zu je 16 Byte ab +0xE40, siehe RA_SLOTINFO in ra_ingame.c. */
#define RA_SLOTINFO_OFS  0x0e40
#define RA_SLOTINFO_LEN  224
/* Uhrzeit des PC, 7 Byte BCD wie SMPC OREG1..7, dahinter 0xA5 als Kennung.
 * Die Saturn nimmt sie fuer Datum und Uhrzeit eines Speicherstands.
 * Gegenstueck: RA_UHR_PC in ra_ingame.c. */
#define TYPE_UHR         0x1A
#define RA_UHR_OFS       0x0fd0

#define RA_ACK_OK        0
#define RA_ACK_ZU_GROSS  1
#define RA_ACK_LAENGE    2
#define RA_ACK_SUMME     3
#define RA_ACK_PFAD      4      /* Datei liess sich nicht anlegen */
#define RA_ACK_FOLGE     5      /* Rahmen ausgelassen */
#define RA_ACK_SCHREIB   6      /* Fehler auf der Karte */
#define RA_ACK_ZUSTAND   7      /* Rahmen passt nicht zum Stand */

static volatile u8 ra_ack_typ = 0;      /* 0 = nichts zu senden */
static volatile u8 ra_ack_status = 0;

/* Dateitransfer: Zustand. Der Interrupt fuellt, der Disc-Thread schreibt. */
#define RA_FILE_BLOCK   4000        /* 16 Rahmen a 250 Byte */
#define RA_FILE_ENDUNG  ".$$$"

#define RA_FZ_AUS       0
#define RA_FZ_START     1           /* BEGIN kam, Datei noch nicht offen */
#define RA_FZ_OFFEN     2

static FIL  ra_file_fp;
static char ra_filebuf[RA_FILE_BLOCK];
static char ra_file_pfad[64];
static volatile int ra_file_zustand = RA_FZ_AUS;
static volatile int ra_file_len = 0;        /* im Puffer wartende Bytes */
static volatile int ra_file_voll = 0;       /* Marke: Block schreiben */
static volatile int ra_file_schluss = 0;    /* Marke: abschliessen */
static volatile u8  ra_file_folge = 0;      /* naechste erwartete Folgenummer */
static volatile u8  ra_file_fehler = RA_ACK_OK;
static u32 ra_file_soll = 0, ra_file_ab = 0;
static u32 ra_file_summe = 0, ra_file_erwartet = 0;

static u8  rx_stand = 0, rx_typ, rx_seq, rx_len, rx_idx;
static u8  rx_payload[250];

static void ra_rx_frame(void)
{
	int i;

	switch(rx_typ){
	case TYPE_HOOK_BEGIN:
		if(rx_len < 18) return;
		ra_hook_dateiname((const char*)rx_payload, ra_hook_name,
		                  sizeof(ra_hook_name));
		ra_hook_soll = ((int)rx_payload[16]<<8) | rx_payload[17];
		ra_hook_len = 0;
		ra_hook_bereit = 0;
		ra_ack_status = (ra_hook_soll > RA_HOOK_MAX) ? RA_ACK_ZU_GROSS : RA_ACK_OK;
		ra_ack_typ = TYPE_HOOK_BEGIN;
		break;

	case TYPE_HOOK_DATA: {
		int off;
		if(rx_len < 3) return;
		off = ((int)rx_payload[0]<<8) | rx_payload[1];
		if(off < 0 || off + (rx_len-2) > RA_HOOK_MAX) return;
		for(i=0; i<rx_len-2; i++) ra_hookbuf[off+i] = (char)rx_payload[2+i];
		if(off + (rx_len-2) > ra_hook_len) ra_hook_len = off + (rx_len-2);
		break;
	}

	case TYPE_HOOK_END: {
		u32 summe = 0, erwartet;
		int laenge;
		if(rx_len < 6) return;
		laenge = ((int)rx_payload[0]<<8) | rx_payload[1];
		erwartet = ((u32)rx_payload[2]<<24) | ((u32)rx_payload[3]<<16) |
		           ((u32)rx_payload[4]<<8)  |  (u32)rx_payload[5];
		for(i=0; i<laenge && i<RA_HOOK_MAX; i++) summe += (u8)ra_hookbuf[i];

		if(laenge != ra_hook_len || laenge != ra_hook_soll){
			ra_ack_status = RA_ACK_LAENGE;
		}else if(summe != erwartet){
			ra_ack_status = RA_ACK_SUMME;
		}else{
			ra_hook_bereit = 1;
			ra_ack_status = RA_ACK_OK;
		}
		ra_ack_typ = TYPE_HOOK_END;
		break;
	}

	case TYPE_STAND: {
		volatile u8 *m = (volatile u8*)RA_MBOX_BASE;
		if(rx_len < 2) return;
		/* Alte Antwort loeschen, damit auch eine gleichlautende neue als
		 * Aenderung erkannt wird. Das Auftragswort so schreiben, dass die
		 * Saturn es nie halb sieht: erst Vorgang und Slot, zuletzt die
		 * Kennung, die sie prueft. */
		m[RA_ANTWORT_OFS+0] = 0; m[RA_ANTWORT_OFS+1] = 0;
		m[RA_ANTWORT_OFS+2] = 0; m[RA_ANTWORT_OFS+3] = 0;
		m[RA_AUFTRAG_OFS+2] = rx_payload[0];
		m[RA_AUFTRAG_OFS+3] = rx_payload[1];
		m[RA_AUFTRAG_OFS+1] = 0x41;     /* 'A' */
		m[RA_AUFTRAG_OFS+0] = 0x53;     /* 'S' */
		break;
	}

	case TYPE_UHR: {
		volatile u8 *m = (volatile u8*)RA_MBOX_BASE;
		if(rx_len < 7) return;
		m[RA_UHR_OFS+7] = 0;            /* waehrend des Schreibens ungueltig */
		for(i=0; i<7; i++)
			m[RA_UHR_OFS+i] = rx_payload[i];
		m[RA_UHR_OFS+7] = 0xA5;
		break;
	}

	/* Dateitransfer. Hier wird nur gesammelt und vorgemerkt; geoeffnet,
	 * geschrieben und geschlossen wird in ra_filexfer_tick(). */
	case TYPE_FILE_BEGIN: {
		int n;
		if(rx_len < 6) return;
		if(ra_file_zustand!=RA_FZ_AUS) return;   /* laeuft schon einer */
		ra_file_soll = ((u32)rx_payload[0]<<24) | ((u32)rx_payload[1]<<16) |
		               ((u32)rx_payload[2]<<8)  |  (u32)rx_payload[3];
		n = rx_len - 4;
		if(n > (int)sizeof(ra_file_pfad)-1) n = sizeof(ra_file_pfad)-1;
		for(i=0; i<n; i++)
			ra_file_pfad[i] = (char)rx_payload[4+i];
		ra_file_pfad[n] = 0;
		ra_file_len = 0;
		ra_file_ab = 0;
		ra_file_summe = 0;
		ra_file_voll = 0;
		ra_file_schluss = 0;
		ra_file_fehler = RA_ACK_OK;
		ra_file_folge = (u8)(rx_seq + 1);
		ra_file_zustand = RA_FZ_START;
		break;
	}

	case TYPE_FILE_DATA:
		if(ra_file_zustand!=RA_FZ_OFFEN) return;
		if(rx_seq!=ra_file_folge){
			/* Ein ausgelassener Rahmen macht die Datei still falsch, also
			 * den ganzen Transfer abbrechen. */
			ra_file_fehler = RA_ACK_FOLGE;
			ra_file_schluss = 1;
			break;
		}
		ra_file_folge = (u8)(rx_seq + 1);
		if(ra_file_len + rx_len > RA_FILE_BLOCK){
			/* Der PC hat die Quittung nicht abgewartet. */
			ra_file_fehler = RA_ACK_ZUSTAND;
			ra_file_schluss = 1;
			break;
		}
		for(i=0; i<rx_len; i++)
			ra_filebuf[ra_file_len + i] = (char)rx_payload[i];
		ra_file_len += rx_len;
		if(ra_file_len >= RA_FILE_BLOCK)
			ra_file_voll = 1;
		break;

	case TYPE_FILE_END:
		if(ra_file_zustand!=RA_FZ_OFFEN) return;
		if(rx_len < 8) return;
		ra_file_soll     = ((u32)rx_payload[0]<<24) | ((u32)rx_payload[1]<<16) |
		                   ((u32)rx_payload[2]<<8)  |  (u32)rx_payload[3];
		ra_file_erwartet = ((u32)rx_payload[4]<<24) | ((u32)rx_payload[5]<<16) |
		                   ((u32)rx_payload[6]<<8)  |  (u32)rx_payload[7];
		ra_file_schluss = 1;
		break;

	default:
		break;
	}
}

int ra_uart_rx_byte(unsigned char b)
{
	switch(rx_stand){
	case 0:
		if(b!=FRAME_MAGIC1) return 0;
		rx_stand = 1;
		return 1;
	case 1:
		if(b!=FRAME_MAGIC2){ rx_stand = 0; return 0; }
		rx_stand = 2;
		return 1;
	case 2:
		rx_typ = b;  rx_stand = 3;  return 1;
	case 3:
		rx_seq = b;  rx_stand = 4;  return 1;
	case 4:
		rx_len = b;
		if(rx_len > sizeof(rx_payload)){ rx_stand = 0; return 1; }
		rx_idx = 0;
		rx_stand = (rx_len==0) ? 6 : 5;
		return 1;
	case 5:
		rx_payload[rx_idx++] = b;
		if(rx_idx >= rx_len) rx_stand = 6;
		return 1;
	case 6: {
		/* CRC-8 ueber TYPE, SEQ, LEN und die Nutzdaten, wie beim Senden. */
		u8 kopf[3];
		u8 c;

		kopf[0] = rx_typ;
		kopf[1] = rx_seq;
		kopf[2] = rx_len;
		c = ra_crc8_cont(ra_crc8(kopf, 3), rx_payload, rx_len);
		if(c == b) ra_rx_frame();
		rx_stand = 0;
		return 1;
	}
	default:
		rx_stand = 0;
		return 0;
	}
}

static void ra_send_ack(void)
{
	u8 buf[9];
	int len = 2, p;

	buf[0] = FRAME_MAGIC1;
	buf[1] = FRAME_MAGIC2;
	buf[2] = TYPE_HOOK_ACK;
	buf[3] = ra_seq++;
	buf[4] = (u8)len;
	buf[5] = ra_ack_typ;
	buf[6] = ra_ack_status;
	p = 7;
	buf[p] = ra_crc8(&buf[2], 3 + len);
	ra_uart_send(buf, p + 1);

	ra_ack_typ = 0;
}

/* Antwort der Saturn auf einen Speicherstand-Auftrag weitergeben, sobald
 * sich das Antwortwort aendert. Ein Lesezugriff je Runde auf ein Wort. */
static void ra_stand_antwort_pruefen(void)
{
	static u8 zuletzt[4];
	volatile u8 *m = (volatile u8*)RA_MBOX_BASE;
	u8 buf[6 + 4 + RA_SLOTINFO_LEN + 1];
	u8 a[4];
	int i, p;

	for(i=0; i<4; i++) a[i] = m[RA_ANTWORT_OFS+i];
	if(a[0]==zuletzt[0] && a[1]==zuletzt[1] && a[2]==zuletzt[2] && a[3]==zuletzt[3])
		return;
	for(i=0; i<4; i++) zuletzt[i] = a[i];
	if(a[0]!=0x53 || a[1]!=0x51)      /* "SQ" */
		return;

	buf[0] = FRAME_MAGIC1;
	buf[1] = FRAME_MAGIC2;
	buf[2] = TYPE_STAND_ANTW;
	buf[3] = ra_seq++;
	buf[4] = (u8)(4 + RA_SLOTINFO_LEN);
	p = 5;
	for(i=0; i<4; i++) buf[p++] = a[i];
	for(i=0; i<RA_SLOTINFO_LEN; i++) buf[p++] = m[RA_SLOTINFO_OFS+i];
	buf[p] = ra_crc8(&buf[2], 3 + 4 + RA_SLOTINFO_LEN);
	ra_uart_send(buf, p + 1);
}

/* Schreibende Haelfte des Dateitransfers. Aufruf aus dem Leerlaufzweig von
 * disk_task() (saturn_main.c), nur dort ist der Kartenzugriff erlaubt. Tut
 * nichts, wenn nichts wartet. */
void ra_filexfer_tick(void)
{
	char tmp[72];
	u32 n;
	int len, i;

	if(ra_file_zustand==RA_FZ_START){
		strcpy(tmp, ra_file_pfad);
		strcat(tmp, RA_FILE_ENDUNG);
		if(f_open(&ra_file_fp, tmp, FA_WRITE|FA_CREATE_ALWAYS)){
			ra_file_zustand = RA_FZ_AUS;
			ra_file_fehler = RA_ACK_PFAD;
		}else{
			ra_file_zustand = RA_FZ_OFFEN;
		}
		ra_ack_status = ra_file_fehler;
		ra_ack_typ = TYPE_FILE_BEGIN;
		return;
	}

	if(ra_file_zustand!=RA_FZ_OFFEN) return;
	if(!ra_file_voll && !ra_file_schluss) return;

	len = ra_file_len;
	if(len>0){
		for(i=0; i<len; i++)
			ra_file_summe += (u8)ra_filebuf[i];
		if(f_write(&ra_file_fp, ra_filebuf, len, &n) || (int)n!=len)
			ra_file_fehler = RA_ACK_SCHREIB;
		ra_file_ab += (u32)len;
		ra_file_len = 0;
	}
	ra_file_voll = 0;

	if(!ra_file_schluss){
		ra_ack_status = ra_file_fehler;
		ra_ack_typ = TYPE_FILE_DATA;
		return;
	}

	f_sync(&ra_file_fp);
	f_close(&ra_file_fp);
	ra_file_zustand = RA_FZ_AUS;
	ra_file_schluss = 0;

	if(ra_file_fehler==RA_ACK_OK){
		if(ra_file_ab!=ra_file_soll)
			ra_file_fehler = RA_ACK_LAENGE;
		else if(ra_file_summe!=ra_file_erwartet)
			ra_file_fehler = RA_ACK_SUMME;
	}

	strcpy(tmp, ra_file_pfad);
	strcat(tmp, RA_FILE_ENDUNG);
	if(ra_file_fehler==RA_ACK_OK){
		/* Erst jetzt das Ziel ersetzen: Bis hierher war eine vorhandene
		 * Firmware unangetastet. */
		f_unlink(ra_file_pfad);
		if(f_rename(tmp, ra_file_pfad))
			ra_file_fehler = RA_ACK_SCHREIB;
		else
			printk("ra: file %s %d bytes\n", ra_file_pfad, (int)ra_file_ab);
	}
	if(ra_file_fehler!=RA_ACK_OK){
		f_unlink(tmp);
		printk("ra: file failed %d\n", (int)ra_file_fehler);
	}

	ra_ack_status = ra_file_fehler;
	ra_ack_typ = TYPE_FILE_END;
}

/* ==========================================================================
 * Aenderungsprotokoll
 *
 * Die Saturn legt in jedem Bild jede geaenderte Watch-Adresse im SDRAM ab, bei
 * RA_PROT_SDRAM (Gegenstueck: RA_PROT_SDRAM in ra_ingame.c). Gelesen wird in
 * jeder Runde (20 ms); beim SDRAM hat die Saturn laut memhub.v Vorrang.
 *
 * Ein erweiterter FPGA mit eigenem Protokollspeicher wird ebenfalls
 * unterstuetzt (RA_PROT_FENSTER in ra_ingame.c). Gelesen wird dann ueber zwei
 * FPGA-Register: 0x60000030 nimmt die Wortnummer (16-Bit-Worte), 0x60000032
 * liefert das Wort dort und zaehlt danach eins weiter. Je 32-Bit-Wort der
 * Saturn erst die obere, dann die untere Haelfte. Aufbau in beiden Faellen:
 *   Wort 0  Zahl aller geschriebenen Datensaetze (zuletzt geschrieben)
 *   Wort 1  Kennung "RAPR"
 *   Wort 2  Dauer der Erfassung im letzten Bild (us)
 *   Wort 3  groesste Dauer seit Spielstart (us)
 *   ab 8    RA_PROT_PLAETZE Datensaetze zu Kopf und Wert, im Kreis
 *
 * Weitergereicht wird jeder Datensatz unveraendert als TYPE_PROT:
 *   Nutzdaten: Merker(1), dann bis zu 31 Datensaetze zu 8 Byte (Kopf, Wert,
 *   big-endian). Merker Bit 0: davor fehlen Datensaetze (Ueberlauf).
 * Kommt die STM32 nicht hinterher, fehlen Datensaetze; das wird gemeldet
 * statt Zerrissenes zu senden. Die Saturn frischt alle Eintraege reihum auf,
 * das Werkzeug ist danach wieder vollstaendig.
 * ========================================================================== */
#define RA_PROT_IDX      (*(volatile u16 *)0x60000030u)
#define RA_PROT_DAT      (*(volatile u16 *)0x60000032u)
#define RA_PROT_KENN     0x52415052u
#define RA_PROT_ERSTER   8
#define RA_PROT_PLAETZE  1020
#define RA_PROT_JE_RAHMEN 31
#define TYPE_PROT        0x1B
#define TYPE_PROT_ZEIT   0x1C

/* Ohne geaenderten FPGA schreibt die Saturn dasselbe Protokoll ins SDRAM,
 * hinter die Mailbox (RA_PROT_SDRAM in ra_ingame.c). Dort traegt Wort 1 die
 * Kennung nur, solange das Protokoll dort laeuft. */
#define RA_PROT_SDRAM    ((volatile u32 *)(RA_MBOX_BASE + 0x2000u))
static int ra_prot_im_sdram = 0;

static u32 ra_prot_wort(u32 k)
{
	u32 hi, lo;
	if (ra_prot_im_sdram)
		return ra_bswap32(RA_PROT_SDRAM[k]);
	RA_PROT_IDX = (u16)(k * 2);
	hi = RA_PROT_DAT;
	lo = RA_PROT_DAT;
	return (hi << 16) | lo;
}

/* Die Saturn schreibt ein Langwort als zwei 16-Bit-Zugriffe. Zweimal gleich
 * gelesen gilt als stehender Wert. */
static int ra_prot_stabil(u32 k, u32 *wert)
{
	int i;
	u32 a = ra_prot_wort(k);
	for (i = 0; i < 8; i++) {
		u32 b = ra_prot_wort(k);
		if (a == b) { *wert = a; return 1; }
		a = b;
	}
	return 0;
}

static u32 ra_prot_gelesen = 0;
static int ra_prot_bekannt = 0;
static u8  ra_prot_luecke = 0;
static int ra_prot_zeit_runde = 0;

static void ra_prot_senden(const u8 *saetze, int n, u8 merker)
{
	u8 buf[6 + RA_FRAME_MAX];
	int len = 1 + n * 8, p;

	buf[0] = FRAME_MAGIC1;
	buf[1] = FRAME_MAGIC2;
	buf[2] = TYPE_PROT;
	buf[3] = ra_seq++;
	buf[4] = (u8)len;
	buf[5] = merker;
	memcpy(&buf[6], saetze, (size_t)(n * 8));
	p = 5 + len;
	buf[p] = ra_crc8(&buf[2], 3 + len);
	ra_uart_send(buf, p + 1);
}

/* Dauer der Erfassung auf der Saturn, etwa jede Sekunde: letzte und groesste,
 * je 4 Byte. Damit zeigt das Werkzeug, was die Erfassung das Spiel kostet. */
static void ra_prot_zeit_senden(void)
{
	u8 buf[6 + 8 + 1];
	u32 d = ra_prot_wort(2), m = ra_prot_wort(3);
	int i;

	buf[0] = FRAME_MAGIC1;
	buf[1] = FRAME_MAGIC2;
	buf[2] = TYPE_PROT_ZEIT;
	buf[3] = ra_seq++;
	buf[4] = 8;
	for (i = 0; i < 4; i++) {
		buf[5 + i] = (u8)(d >> (24 - 8 * i));
		buf[9 + i] = (u8)(m >> (24 - 8 * i));
	}
	buf[13] = ra_crc8(&buf[2], 3 + 8);
	ra_uart_send(buf, 14);
}

static void ra_prot_pruefen(void)
{
	u8 saetze[RA_PROT_JE_RAHMEN * 8];
	u32 z;

	/* Quelle waehlen: SDRAM, wenn die Saturn es dort fuehrt, sonst das
	 * FPGA-Fenster. Beim Wechsel neu aufsetzen. */
	{
		const int sdram = ra_bswap32(RA_PROT_SDRAM[1]) == RA_PROT_KENN;
		if (sdram != ra_prot_im_sdram) {
			ra_prot_im_sdram = sdram;
			ra_prot_bekannt = 0;
		}
	}

	if (ra_prot_wort(1) != RA_PROT_KENN) {
		ra_prot_bekannt = 0;
		return;
	}
	if (!ra_prot_stabil(0, &z))
		return;

	/* Neustart des Spiels: Zaehler beginnt wieder bei 0. */
	if (!ra_prot_bekannt || z < ra_prot_gelesen) {
		ra_prot_gelesen = 0;
		ra_prot_bekannt = 1;
	}
	if (z - ra_prot_gelesen > RA_PROT_PLAETZE - 32) {
		ra_prot_gelesen = z - (RA_PROT_PLAETZE / 2);
		ra_prot_luecke = 1;
	}

	if (++ra_prot_zeit_runde >= 50) {
		ra_prot_zeit_runde = 0;
		ra_prot_zeit_senden();
	}

	while (ra_prot_gelesen != z) {
		u32 start = ra_prot_gelesen, z2;
		int n = 0, i;

		while (n < RA_PROT_JE_RAHMEN && ra_prot_gelesen + (u32)n != z) {
			u32 platz = (start + (u32)n) % RA_PROT_PLAETZE;
			u32 kopf = ra_prot_wort(RA_PROT_ERSTER + platz * 2);
			u32 wert = ra_prot_wort(RA_PROT_ERSTER + platz * 2 + 1);
			for (i = 0; i < 4; i++) {
				saetze[n * 8 + i]     = (u8)(kopf >> (24 - 8 * i));
				saetze[n * 8 + 4 + i] = (u8)(wert >> (24 - 8 * i));
			}
			n++;
		}

		/* Hat die Saturn waehrenddessen ueber das Gelesene geschrieben? */
		if (!ra_prot_stabil(0, &z2))
			return;
		if (z2 - start > RA_PROT_PLAETZE - 32) {
			ra_prot_gelesen = z2 - (RA_PROT_PLAETZE / 2);
			ra_prot_luecke = 1;
			z = z2;
			continue;
		}

		ra_prot_senden(saetze, n, ra_prot_luecke);
		ra_prot_luecke = 0;
		ra_prot_gelesen = start + (u32)n;
	}
}

void ra_mailbox_task(void *arg)
{
	static ra_entry_t entries[RA_MAX_ENTRIES];
	/* Startwert ohne Bedeutung: have_sent_once erzwingt, dass der erste
	 * gueltige Stand sofort gesendet wird. */
	u32 last_sent_frame_count = 0;
	int have_sent_once = 0;
	int stale_polls = 0;
	u32 gi_sent_gen = 0;
	int gi_polls = 0;
	u32 tx_gemeldet = 0;

	(void)arg;

	for (;;) {
		u32 frame_count;
		int n;

		if (uart4_tc_frueh != tx_gemeldet) {
			tx_gemeldet = uart4_tc_frueh;
			printk("ra: tx cut %d\n", (int)tx_gemeldet);
		}

		/* Quittung an den PC. Im Interrupt nur vorgemerkt, gesendet wird
		 * hier. */
		if (ra_ack_typ) ra_send_ack();
		ra_stand_antwort_pruefen();

		/* Spielerkennung zuerst und unabhaengig von der Mailbox. Neues Spiel:
		 * sofort senden. Sonst regelmaessig wiederholen, damit auch ein PC,
		 * der erst mitten im Spiel verbindet, schnell weiss, was laeuft. */
		if (ra_gameinfo_valid) {
			if (ra_gameinfo_gen != gi_sent_gen) {
				u32 gesendet = ra_send_gameinfo();
				gi_polls = 0;
				if (gesendet) gi_sent_gen = gesendet;
			} else if (++gi_polls >= RA_GI_RESEND_POLLS) {
				gi_polls = 0;
				ra_send_gameinfo();
			}
		}

		/* Aenderungsprotokoll in jeder Runde, siehe RA_PROT_IDX. */
		ra_prot_pruefen();

		/* Mailbox nur jede RA_LESE_TEILER-te Runde anfassen. */
		{
			static int lese_runde = 0;
			if (++lese_runde < RA_LESE_TEILER) {
				osDelay(RA_POLL_MS);
				continue;
			}
			lese_runde = 0;
		}

		if (!ra_mailbox_valid()) {
			/* Kein Hook-Satz fuer dieses Spiel: nichts senden. */
			have_sent_once = 0;
			stale_polls = 0;
			osDelay(RA_POLL_MS);
			continue;
		}

		n = ra_read_snapshot(&frame_count, entries, RA_MAX_ENTRIES);
		if (n >= 0) {
			if (!have_sent_once || frame_count != last_sent_frame_count) {
				ra_send_watch_report(frame_count, entries, n);
				last_sent_frame_count = frame_count;
				have_sent_once = 1;
				stale_polls = 0;
			} else if (++stale_polls >= RA_STALE_PING_POLLS) {
				/* Der Rundenzaehler steht: trotzdem gelegentlich senden, als
				 * Lebenszeichen fuer die Stillstandserkennung des PC. */
				ra_send_watch_report(frame_count, entries, n);
				stale_polls = 0;
			}
		}
		/* n < 0: kein konsistenter Stand, dieser Zyklus wird ausgelassen. */

		osDelay(RA_POLL_MS);
	}
}

void ra_mailbox_start(void)
{
	osThreadAttr_t attr;

	/* Beim Einschalten melden, welcher Stand laeuft, mit Uhrzeit des Baus. */
	printk("ra: mcuapp " __DATE__ " " __TIME__ "\n");

	memset(&attr, 0, sizeof(attr));
	attr.priority = osPriorityLow;
	attr.stack_size = 1024;  /* buf[256] in ra_send_watch_report */
	osThreadNew(ra_mailbox_task, NULL, &attr);
}
