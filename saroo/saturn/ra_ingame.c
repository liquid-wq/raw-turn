/*
 * RAW-TURN: RetroAchievements-Anbindung der Saturn-Firmware.
 *
 * Beim Spielstart (ra_ingame_install, aus read_1st in game_load.c) haengt sich
 * die Firmware hinter die V-Blank-OUT- und die SMPC-Routine des Spiels. In
 * jedem Bild liest sie danach die Speicherstellen, die das PC-Werkzeug fuer
 * das laufende Spiel braucht, und legt sie ab:
 *
 *   - im Aenderungsprotokoll: jede geaenderte Stelle jedes Bildes, im SDRAM
 *     der SAROO, siehe RA_PROT_SDRAM;
 *   - in der Mailbox im SDRAM der SAROO, reihum acht Stellen je Bild. Daraus
 *     liest das Werkzeug die Adressliste und erkennt, dass der Hook laeuft.
 *
 * Code und Daten, die in jedem Bild laufen (Sektionen .wramtext und
 * .wramdata), liegen in der Kartusche, siehe ldscript_kartusche. Das Work RAM
 * bleibt ganz beim Spiel. Die Firmware laeuft mit dem offiziellen FPGA der
 * SAROO. Ein erweiterter FPGA mit eigenem Block-RAM (Kennung "RA") wird
 * erkannt und dann fuer Arbeitsblock und Protokoll genutzt.
 *
 * Speicherstaende
 * ---------------
 * L + R + Unten legt einen Schnellstand an, das Werkzeug kann ausserdem in
 * vier feste Slots speichern und jeden Stand laden. Datei
 * /SAROO/rastate_<Kennung>_<Slot>.bin:
 *   Kopf (0x40 Byte), Work-RAM High und Low, VDP1-VRAM, VDP2-VRAM, CRAM.
 * Die CPU-Register stecken im Stapel des Spiels (ra_ingame_s.S), der Stapel
 * im Work-RAM. Laden schreibt alles zurueck und kehrt ueber den gesicherten
 * Stapelzeiger in das Spiel zurueck.
 *
 * Nicht enthalten: VDP2-Register (nur beschreibbar; SGL-Spiele stellen sie
 * aus ihrer Kopie im Work-RAM selbst wieder her), Sound-CPU samt Sound-RAM,
 * CD-Zustand der SAROO und Slave-SH-2.
 *
 * Die Datei entsteht in Stuecken ueber write_file(); die SAROO puffert jedes
 * Stueck in ihrem SDRAM. Der Kopf wird zuletzt geschrieben: Bricht das
 * Sichern ab, bleibt der Slot leer statt halb gefuellt.
 */

#include "main.h"
#include "smpc.h"
#include "vdp2.h"

void ra_vbo_entry(void);                            /* ra_ingame_s.S */
void ra_vbo_schlank(void);                          /* ra_ingame_s.S */
extern u32 ra_vbo_schlank_ziel;
#define RA_VBO_EINSTIEG ((u32)ra_vbo_schlank)
void ra_smpc_entry(void);
void ra_fortsetzen(u32 sp);
void ra_eigener_stapel(void (*f)(int), int arg);

/* Variablen, die in jedem Bild gelesen werden (.wramdata). Ein Anfangswert
 * waere wirkungslos, gesetzt wird alles in ra_ingame_install(). */
#define RA_IM_WRAMDATA  __attribute__((section(".wramdata")))

/* Bildzaehler seit dem Spielstart, zugleich die Spielzeit eines Stands. */
RA_IM_WRAMDATA static u32 ra_nurra_takt;

/* Wird von ra_ingame_s.S gelesen: Ungleich 0 haelt die vollstaendige
 * Vorschaltung das Spiel an. Hier immer 0. */
RA_IM_WRAMDATA u32 ra_menue_offen;

RA_IM_WRAMDATA u32 ra_vbo_next;         /* bisheriger V-Blank-OUT-Handler */
u32 ra_smpc_next;                       /* bisheriger SMPC-Handler */
u32 ra_vbi_next;                        /* bisheriger V-Blank-IN-Handler */
void ra_vbi_entry(void);                /* ra_ingame_s.S */
u32 ra_timer_next;                      /* bisheriger SCU-Timer-0-Handler */

RA_IM_WRAMDATA static int ra_timer_an;  /* bleibt 0, siehe ra_timer_nachlauf */

static void ra_watch_schreiben(void);
#define SMPC_VEKTOR     ((u32*)0x0600011c)  /* Vektor 0x47 */
#define VBI_VEKTOR      ((u32*)0x06000100)  /* Vektor 0x40 */
#define VBO_VEKTOR      ((u32*)0x06000104)  /* Vektor 0x41 */

/* 1 nach einem Speichern oder Laden, bis die Tastenkombination losgelassen
 * gemeldet wird. Siehe ra_ingame_pruefen(). */
RA_IM_WRAMDATA static int ra_kombi_gesperrt;
/* Auftrag fuer ra_ingame_menue(): Vorgang<<8 | Slot, 0 = keiner. */
RA_IM_WRAMDATA static u32 ra_auftrag_jetzt;

/* L + R + Unten legt einen Schnellstand an. */
#define PAD_KOMBI_LR    (PAD_LT | PAD_RT | PAD_DOWN)

/* --- Speicherstand ------------------------------------------------------ */

#define ST_MAGIC        0x52415354      /* "RAST" */
#define ST_VERSION      2
#define ST_KOPF         0x40
#define ST_STUECK       0x40000         /* passt in den Puffer der SAROO */
#define SLOTS           4
/* Schnellstaende: fortlaufend nummeriert (X.1, X.2, ...), die letzten SLOTS_X
 * bleiben erhalten, ein neuer ersetzt den aeltesten. Ringplatz p liegt in
 * slot_kopf[SLOTS+p], Datei rastate_<Kennung>_X<p+1>.bin; die fortlaufende
 * Nummer steht im Kopf (folge). */
#define SLOTS_X         10
#define SLOTS_ALLE      (SLOTS+SLOTS_X)
#define SLOT_X_NEU      0xff            /* Auftrag: naechster Schnellstand */

typedef struct {
	u32 adresse;
	u32 laenge;
} BEREICH;

/* Ungecacht gelesen: Was das Spiel per DMA ins Work-RAM schreibt, geht am
 * Cache vorbei. */
static const BEREICH st_bereiche[] = {
	{ 0x26000000, 0x100000 },           /* Work-RAM High */
	{ 0x20200000, 0x100000 },           /* Work-RAM Low */
	{ 0x25C00000, 0x080000 },           /* VDP1-VRAM */
	{ 0x25E00000, 0x080000 },           /* VDP2-VRAM */
	{ 0x25F00000, 0x001000 },           /* CRAM, wortweise */
};
#define ST_BEREICHE     (sizeof(st_bereiche)/sizeof(st_bereiche[0]))
#define ST_CRAM         4

typedef struct {
	u32  magic;
	u32  version;
	char kennung[16];
	u8   zeit[7];                       /* SMPC OREG1..7 */
	u8   frei;
	u32  sp;
	u32  folge;                         /* Schnellstand: fortlaufende Nummer, sonst 0 */
	u32  bilder;                        /* Spielzeit in Bildern seit dem Spielstart */
	u8   pal;                           /* 1: 50 Bilder/s, 0: 60 */
	u8   rest[ST_KOPF-0x2D];
} ST_KOPF_T;

static char ra_kennung[17];
static u32  ra_sp;                      /* Stapelzeiger des Spiels, aus ra_ingame_s.S */

/* Statuswort fuer das PC-Werkzeug, Mailbox +0x04. Das Werkzeug kann ueber den
 * Hook-Satz RA_WUNSCH_SPERRE hineinschreiben, dann sind Speichern und Laden
 * gesperrt. Beim Spielstart ersetzt die Firmware den Wunsch durch ihren
 * Status; RA_ST_GELADEN wird gesetzt, sobald ein Stand geladen wurde. */
#define RA_STATUS_WORT      ((u32*)0x223F0004)
#define RA_WUNSCH_SPERRE    0x52480001      /* "RH", 1 */
#define RA_STATUS_KENN      0x52530000      /* "RS" */
#define RA_ST_GESPERRT      0x0001
#define RA_ST_GELADEN       0x0002

/* Speichern und Laden im Auftrag des PC-Werkzeugs.
 *
 *   Auftrag (PC -> STM32 -> hier), Mailbox +0xFE0:
 *       0x5341 << 16 | Vorgang << 8 | Slot      ("SA")
 *   Antwort (hier -> STM32 -> PC), Mailbox +0xFE4:
 *       0x5351 << 16 | Vorgang << 8 | Ergebnis  ("SQ")
 *   Slot-Uebersicht, siehe RA_SLOTINFO.
 *   Slot 0..3 fest, 4..13 Schnellstaende (Ring), 0xFF = naechster Schnellstand.
 *
 * Gelesen wird der Auftrag alle acht Bilder. */
#define RA_AUFTRAG_WORT     ((volatile u32*)0x223F0FE0)
#define RA_ANTWORT_WORT     ((volatile u32*)0x223F0FE4)
/* 14 Eintraege zu je 16 Byte ab +0xE40: Byte 0 belegt, 1..7 Uhrzeit, 8..9
 * Nummer (Schnellstand), 10..13 Spielzeit in Bildern, 14 PAL, 15 frei. */
#define RA_SLOTINFO         ((volatile u8*)0x223F0E40)
#define RA_SLOTINFO_EINTRAG 16
#define RA_AUFTRAG_KENN     0x5341
#define RA_ANTWORT_KENN     0x5351
#define RA_VG_SPEICHERN     1
#define RA_VG_LADEN         2
#define RA_VG_SLOTS         3
#define RA_ERG_OK           1
#define RA_ERG_FEHLER       2
#define RA_ERG_LEER         3
#define RA_ERG_GESPERRT     4
#define RA_ERG_UNBEKANNT    5

/* Mailbox im SDRAM der SAROO, vom Hook-Satz befuellt. */
#define RA_MBOX             ((volatile u32*)0x223F0000)
#define RA_MBOX_SEQ         2               /* +0x08 Rundenzaehler */
#define RA_MBOX_ANZAHL      3               /* +0x0C, obere 16 Bit */
#define RA_MBOX_EINTRAEGE   4               /* +0x10, je 3 Worte */
/* Wie kFirmwareMaxEintraege im Werkzeug und RA_MAX_ENTRIES auf der STM32. */
#define RA_MAX_WATCH        300
/* Spielkennung im Hook-Satz, 16 Byte. Gegenstueck: kKennungOffset im
 * Werkzeug (core/mailbox.h). */
#define RA_MBOX_KENNUNG     0x223F0FF0

static u32 ra_watch_adr[RA_MAX_WATCH];
static int ra_watch_n;
static u32 ra_watch_seq;

/* Code, der in jedem Bild laeuft, kommt in diese Sektion. Mit
 * ldscript_kartusche liegt sie im Kartuschenspeicher an ihrem Platz. Ein
 * Linkerskript, das sie an eine andere Adresse legt, laesst sie beim
 * Spielstart dorthin kopieren (ra_wram_code_kopieren). Eine Funktion in
 * .wramtext darf im Bildtakt nichts aufrufen, was nicht ebenfalls dort liegt. */
#define RA_IM_WRAM      __attribute__((section(".wramtext")))

/* Arbeitsblock im Block-RAM eines erweiterten FPGA, endet bei 0x02FFEC00. */
#define RA_WRAM_ADR     0x02FFE100u
#define RA_WRAM_KENN    0x52415742u     /* "RAWB" */
#define RA_WRAM_WACHE   0xA5A5A5A5u

typedef struct {
	u32 kennung;
	u32 wache_vorn;
	u32 watch_adr[RA_MAX_WATCH];
	u32 watch_letzt[RA_MAX_WATCH];
	int watch_n;
	int watch_pos;
	int watch_alles;
	u32 watch_seq;
	u32 watch_runde;
	u32 takt;
	u32 wtakt;
	int gehalten;
	int pad_letzt;
	u32 vdp1_bilder;
	u32 prot_bild;                  /* Aenderungsprotokoll, siehe RA_PROT_FENSTER */
	u32 prot_zaehler;
	u32 prot_platz;
	int prot_auffrisch;
	u32 wache_hinten;
} ra_wram_t;

typedef char ra_wram_passt[(sizeof(ra_wram_t) <= 0xB00) ? 1 : -1];

/* AENDERUNGSPROTOKOLL
 *
 * In jedem Bild wird jede Watch-Adresse gelesen und jede Aenderung als
 * Datensatz abgelegt: im SDRAM der SAROO hinter der Mailbox, mit erweitertem
 * FPGA in einem Fenster dort. Die STM32 liest es und reicht es weiter; das
 * Werkzeug wertet Bild fuer Bild aus. Damit sieht es auch Wechsel, die nur
 * ein Bild lang dauern.
 *
 * Aufbau (32-Bit-Worte, ungecacht, die Saturn liest hier nie zurueck):
 *   [0]  Zahl aller bisher geschriebenen Datensaetze, als LETZTES je Bild
 *        geschrieben. Was davor steht, ist vollstaendig.
 *   [1]  Kennung "RAPR"
 *   [2]  Dauer der Erfassung im letzten Bild in us
 *   [3]  groesste Dauer seit Spielstart
 *   [8]  ab hier RA_PROT_PLAETZE Datensaetze zu zwei Worten, im Kreis:
 *        Kopf  = Eintrag<<16 | Bild (untere 16 Bit)
 *        Wert  = gelesenes Wort
 *        Eintrag 0xFFFF schliesst ein Bild ab, Wert = volle Bildnummer.
 * Zusaetzlich gehen je Bild RA_PROT_AUFFRISCH Eintraege auch ohne Aenderung
 * hinaus. Damit kommt das Werkzeug nach einer Luecke (Ueberlauf, spaeter
 * verbunden) in kurzer Zeit wieder zu einem vollstaendigen Bild. */
/* Im SDRAM; die STM32 liest es dort direkt (0x613F2000). */
#define RA_PROT_SDRAM       ((volatile u32*)0x223F2000)
/* Fenster eines erweiterten FPGA, fuer die Saturn nur beschreibbar. */
#define RA_PROT_FENSTER     ((volatile u32*)0x22FFC000)
#define RA_PROT_KENN_WORT   1
#define RA_PROT_KENN        0x52415052u     /* "RAPR" */
#define RA_PROT_ERSTER      8
#define RA_PROT_PLAETZE     1020
#define RA_PROT_ENDE        0xFFFFu
#define RA_PROT_AUFFRISCH   8
/* Untere Haelfte des 1-MHz-Zeitgebers eines erweiterten FPGA, bei +0x38 (in
 * CS2 kommt beim Lesen A1 nicht an, deshalb eine durch 4 teilbare Adresse). */
#define RA_PROT_TICK        (*(volatile u16*)0x25897038)
RA_IM_WRAMDATA static int ra_prot_an;
RA_IM_WRAMDATA static u32 ra_prot_max;
RA_IM_WRAMDATA static volatile u32 *ra_prot_p;  /* FPGA-Fenster oder SDRAM */
RA_IM_WRAMDATA static int ra_prot_fpga;         /* 1: Zeitgeber im FPGA vorhanden */

/* Arbeitsblock im Kartuschenspeicher. Der Normalfall; mit erweitertem FPGA
 * der Rueckfall, falls dessen Block-RAM beschaedigt wird. */
static ra_wram_t ra_w_ersatz;

/* Der gerade benutzte Arbeitsblock. Gesetzt in ra_ingame_install() und
 * ra_watch_laden(). */
RA_IM_WRAMDATA static ra_wram_t *ra_w;

/* Zahl der Faelle, in denen ein Waechterwort nicht mehr stimmte. */
static u32 ra_wram_verloren;

/* Nur Work-RAM, und nur ausgerichtet: Ein Zugriff anderswo kann ein Register
 * mit Lese-Nebenwirkung treffen oder einen Busfehler ausloesen, ein
 * unausgerichteter einen Adressfehler.
 *
 * Immer eingebettet: ra_watch_laden() ruft sie auf, bevor der Bildtakt-Code
 * kopiert ist. Eine eigene Funktion in .wramtext koennte zu diesem Zeitpunkt
 * noch leer sein. */
static inline __attribute__((always_inline)) int ra_watch_adresse_ok(u32 a)
{
	u32 b = a & 0x0fffffff;             /* Cache-Spiegel ausblenden */

	if(a & 3)
		return 0;
	return (b>=0x06000000 && b<0x06100000) || (b>=0x00200000 && b<0x00300000);
}

/* Vom Linker gesetzt, siehe ldscript_kartusche. */
extern char wram_start[], wram_end[], wram_lma[];

/* Cache Control Register des SH7604. Bit 4 (CP) verwirft beim Schreiben alle
 * Eintraege des 4-KByte-Caches. */
#define SH2_CCR         (*(volatile u8*)0xFFFFFE92)

/* read_file() mit anschliessendem Verwerfen des Caches. Liegt der Zielpuffer
 * in der Kartusche, schreibt die STM32 direkt ins SDRAM; ueber den Cache saehe
 * die Saturn danach alte Daten. Der Cache des SH-2 schreibt sofort durch,
 * Verwerfen verliert nichts. Die Einstellung des Caches bleibt unveraendert. */
static int ra_lesen(char *name, int offset, int laenge, void *puffer)
{
	int n = read_file(name, offset, laenge, puffer);
	SH2_CCR = (u8)(SH2_CCR | 0x10);
	return n;
}

/* Den Bildtakt-Code an seinen Platz bringen, einmal je Spielstart.
 *
 * Mit ldscript_kartusche ist .wramtext leer, der Code steht schon in der
 * Kartusche. Ein Linkerskript, das ihn ins Block-RAM eines erweiterten FPGA
 * legt, braucht dessen Kennung "RA" bei +0x30 und Stand ab 0x000a bei +0x34;
 * die Gegenprobe liest ueber den ungecachten Alias, danach wird der Cache
 * verworfen.
 *
 * Rueckgabe 1, wenn der Code an seinem Platz steht. */
static int ra_wram_code_kopieren(void)
{
	u32 laenge = (u32)(wram_end - wram_start);
	volatile u32 *ziel = (volatile u32*)wram_start;
	const u32 *quelle = (const u32*)wram_lma;
	u32 i;
	const u16 kenn = *(volatile u16*)0x25897030;
	const u16 vers = *(volatile u16*)0x25897034;

	/* Laenge 0: Der Bildtakt-Code ist in die Kartusche gelinkt. */
	if(laenge == 0)
		return 1;
	if(kenn != 0x5241 || vers < 0x000a)
		return 0;
	if(laenge > 0x1400)
		return 0;
	for(i=0; i<laenge/4; i++)
		ziel[i] = quelle[i];
	for(i=0; i<laenge/4; i++){
		if(((volatile u32*)((u32)ziel | 0x20000000))[i] != quelle[i])
			return 0;
	}
	SH2_CCR = (u8)(SH2_CCR | 0x10);
	return 1;
}

/* Einen Arbeitsblock mit dem aktuellen Hook-Satz fuellen. Laeuft beim
 * Spielstart, nicht im Bildtakt. */
static void ra_wram_fuellen(ra_wram_t *w)
{
	int k;

	w->kennung = RA_WRAM_KENN;
	w->wache_vorn = RA_WRAM_WACHE;
	w->wache_hinten = RA_WRAM_WACHE;
	for(k=0; k<RA_MAX_WATCH; k++){
		w->watch_adr[k]   = ra_watch_adr[k];
		w->watch_letzt[k] = 0;
	}
	w->watch_n = ra_watch_n;
	w->watch_pos = 0;
	w->watch_alles = 1;
	w->watch_seq = ra_watch_seq;
	w->watch_runde = 0;
	w->takt = 0;
	w->wtakt = 0;
	w->gehalten = 0;
	w->pad_letzt = -1;
	w->vdp1_bilder = 0;
	w->prot_bild = 0;
	w->prot_zaehler = 0;
	w->prot_platz = 0;
	w->prot_auffrisch = 0;
}

/* Watch-Adressen aus der Mailbox uebernehmen, beim Spielstart nach
 * patch_game().
 *
 * Ohne Hook-Satz oder mit dem Satz eines anderen Spiels geschieht nichts: Die
 * Mailbox liegt im SDRAM und uebersteht Reset und Spielwechsel. Verglichen
 * wird die Kennung bei +0xFF0 mit der des laufenden Spiels im Disc-Kopf
 * (0x06002020). */
static void ra_watch_laden(void)
{
	u32 n;
	int i;

	ra_watch_n = 0;
	ra_prot_an = 0;
	if(RA_MBOX[0] != 0x52415341)        /* "RASA": kein Hook-Satz */
		return;
	{
		const u8 *soll = (const u8*)(RA_MBOX_KENNUNG);
		const u8 *ist  = (const u8*)0x06002020;
		int i;
		for(i=0; i<16; i++){
			if(soll[i] != ist[i])
				return;
		}
	}

	n = RA_MBOX[RA_MBOX_ANZAHL] >> 16;
	if(n > RA_MAX_WATCH)
		n = RA_MAX_WATCH;
	for(i=0; i<(int)n; i++){
		u32 a = RA_MBOX[RA_MBOX_EINTRAEGE + i*3];
		if((a & 0x0ffff000) == 0x023f0000)
			a = 0x223f0000 | (a & 0xfff);   /* Statuswort: ungecacht */
		else if(!ra_watch_adresse_ok(a))
			a = 0;
		else
			a |= 0x20000000;                /* Spielspeicher ungecacht lesen */
		ra_watch_adr[i] = a;
		/* Wertfeld auf null, damit die STM32 bis zum ersten Schreiben keinen
		 * alten SDRAM-Inhalt als Messwert weitergibt. */
		RA_MBOX[RA_MBOX_EINTRAEGE + i*3 + 2] = 0;
	}
	ra_watch_n = (int)n;
	ra_watch_seq = RA_MBOX[RA_MBOX_SEQ] & ~1u;

	/* Arbeitsblock anlegen: zuerst eine Schreib-Lese-Probe im Block-RAM eines
	 * erweiterten FPGA.
	 * Beide Bloecke werden gefuellt, der Wechsel auf den Rueckfall im
	 * Bildtakt ist dann ein einziger Zeigerzugriff. */
	{
		volatile u32 *probe = (volatile u32*)RA_WRAM_ADR;
		u32 alt = probe[0];
		int schnell;

		schnell = 0;
		if(*(volatile u16*)0x25897030 == 0x5241 && *(volatile u16*)0x25897034 >= 0x000a){
			probe[0] = 0x5a5aa5a5u;
			schnell = (((volatile u32*)(RA_WRAM_ADR | 0x20000000))[0] == 0x5a5aa5a5u);
			probe[0] = alt;
		}

		ra_wram_fuellen(&ra_w_ersatz);
		if(schnell){
			ra_wram_fuellen((ra_wram_t*)RA_WRAM_ADR);
			ra_w = (ra_wram_t*)RA_WRAM_ADR;
		}else{
			ra_w = &ra_w_ersatz;
		}
		/* Aenderungsprotokoll: im SDRAM, mit erweitertem FPGA (Stand ab
		 * 0x000c) in dessen Fenster. Kopf loeschen: Der Zaehler beginnt
		 * bei 0, daran erkennt die STM32 den Neustart. Die Kennung im SDRAM
		 * gibt es nur, wenn das Protokoll dort laeuft; die Mailbox uebersteht
		 * Spielwechsel, eine alte Kennung muss deshalb weg. */
		{
			int k;
			ra_prot_fpga = schnell && *(volatile u16*)0x25897034 >= 0x000c;
			ra_prot_p = ra_prot_fpga ? RA_PROT_FENSTER : RA_PROT_SDRAM;
			for(k=0; k<RA_PROT_ERSTER; k++)
				RA_PROT_SDRAM[k] = 0;
			for(k=0; k<RA_PROT_ERSTER; k++)
				ra_prot_p[k] = 0;
			ra_prot_p[RA_PROT_KENN_WORT] = RA_PROT_KENN;
			ra_prot_max = 0;
			ra_prot_an = 1;
		}
	}
	ra_wram_verloren = 0;
}

/* Watch-Eintraege, die je Bild in die Mailbox gehen (reihum). */
#define RA_JE_BILD      8

/* Prueft die Waechterworte des Arbeitsblocks. 0 = das Spiel hat hineingeschrieben. */
RA_IM_WRAM static int ra_wram_heil(ra_wram_t *w)
{
	if(w->kennung != RA_WRAM_KENN)
		return 0;
	return (w->wache_vorn == RA_WRAM_WACHE) && (w->wache_hinten == RA_WRAM_WACHE);
}

RA_IM_WRAM static void ra_mbox_legen(volatile u32 *ziel, u32 wert)
{
	*ziel = wert;
}

/* Einen Eintrag in die Mailbox schreiben, im Rundlauf.
 *
 * Geschrieben wird nur, was sich geaendert hat, und in jeder achten Runde
 * alles, damit die Mailbox auch gefuellt ist, wenn das Werkzeug spaeter
 * verbindet. Am Ende jeder Runde steigt der Rundenzaehler um 2 (bleibt gerade). */
RA_IM_WRAM static void ra_watch_schreiben(void)
{
	ra_wram_t *w = ra_w;
	int pos;

	if(!ra_wram_heil(w)){
		/* Auf den Rueckfall umschalten; der ist beim Spielstart fertig
		 * gefuellt worden. */
		ra_w = &ra_w_ersatz;
		ra_wram_verloren += 1;
		return;
	}
	if(w->watch_n <= 0)
		return;

	pos = w->watch_pos;
	if(pos < w->watch_n){
		u32 a = w->watch_adr[pos];
		u32 v;

		/* Adresse bei jedem Zugriff pruefen: Der Arbeitsblock kann
		 * ueberschrieben worden sein, bevor die Waechter es melden. */
		v = (a && ra_watch_adresse_ok(a)) ? *(volatile u32*)a : 0;

		if(w->watch_alles || v != w->watch_letzt[pos] ||
		   (w->watch_runde & 7) == 0){
			w->watch_letzt[pos] = v;
			ra_mbox_legen(&RA_MBOX[RA_MBOX_EINTRAEGE + pos*3 + 2], v);
		}
		w->watch_pos = pos + 1;
	}else{
		w->watch_seq += 2;
		ra_mbox_legen(&RA_MBOX[RA_MBOX_SEQ], w->watch_seq);
		w->watch_pos = 0;
		w->watch_runde += 1;
		w->watch_alles = 0;
	}
}

/* Ein Bild ins Aenderungsprotokoll, siehe RA_PROT_SDRAM. Laeuft in jedem
 * Bild nach der Routine des Spiels und schreibt nur ins Protokoll und in den
 * Arbeitsblock. */
RA_IM_WRAM static void ra_prot_bild(void)
{
	ra_wram_t *w = ra_w;
	volatile u32 *p = ra_prot_p;
	/* Den Zeitgeber gibt es nur im erweiterten FPGA. Beim offiziellen
	 * koennte ein Lesezugriff in dessen Registerfenster Nebenwirkungen
	 * haben, dort wird er deshalb gar nicht angefasst. */
	const int fpga = ra_prot_fpga;
	const u16 t0 = fpga ? RA_PROT_TICK : 0;
	u32 platz, zaehler, bild, dt;
	int pos, n, auf;

	if(!ra_wram_heil(w))
		return;                 /* ra_watch_schreiben() schaltet um */
	n = w->watch_n;
	if(n <= 0)
		return;

	platz   = w->prot_platz;
	zaehler = w->prot_zaehler;
	bild    = w->prot_bild + 1;
	auf     = w->prot_auffrisch;

	for(pos=0; pos<n; pos++){
		const u32 a = w->watch_adr[pos];
		const u32 v = (a && ra_watch_adresse_ok(a)) ? *(volatile u32*)a : 0;
		if(v != w->watch_letzt[pos] || bild == 1 ||
		   (pos >= auf && pos < auf + RA_PROT_AUFFRISCH)){
			w->watch_letzt[pos] = v;
			p[RA_PROT_ERSTER + platz*2]     = ((u32)pos << 16) | (bild & 0xffff);
			p[RA_PROT_ERSTER + platz*2 + 1] = v;
			if(++platz >= RA_PROT_PLAETZE)
				platz = 0;
			zaehler++;
		}
	}
	p[RA_PROT_ERSTER + platz*2]     = (RA_PROT_ENDE << 16) | (bild & 0xffff);
	p[RA_PROT_ERSTER + platz*2 + 1] = bild;
	if(++platz >= RA_PROT_PLAETZE)
		platz = 0;
	zaehler++;

	auf += RA_PROT_AUFFRISCH;
	if(auf >= n)
		auf = 0;
	w->prot_auffrisch = auf;
	w->prot_platz   = platz;
	w->prot_zaehler = zaehler;
	w->prot_bild    = bild;

	p[0] = zaehler;             /* zuletzt: alles davor ist vollstaendig */

	/* Die Saturn liest das Protokoll nie zurueck, das Maximum steht deshalb
	 * in einer eigenen Variable. */
	if(!fpga)
		return;
	dt = (u16)(RA_PROT_TICK - t0);
	p[2] = dt;
	if(dt > ra_prot_max){
		ra_prot_max = dt;
		p[3] = dt;
	}
}

static int  ra_gesperrt;

static ST_KOPF_T slot_kopf[SLOTS_ALLE];

static u16  cram_puffer[0x800];         /* 0x1000 Byte CRAM, wortweise */


/* ======================================================================== */
/* SMPC                                                                       */
/* ======================================================================== */

/* Tasten aus dem letzten INTBACK-Ergebnis des Spiels. Liest nur, -1 wenn
 * nichts Verwertbares dasteht.
 *
 * Wer nur die Controller anfordert, bekommt sie ab OREG0; wer Systemstatus und
 * Controller zusammen anfordert, bekommt erst sieben Byte Status und danach
 * die Controller ab OREG7. Geprueft werden genau diese zwei Stellen, die
 * zuletzt gefundene zuerst.
 *
 * Ein Block sieht so aus:
 *   Anschluss-Byte 0xF1..0xF6  (0xF0 = nichts angeschlossen)
 *   Kennung        (Art<<4)|Groesse, Digital- und 3D-Pad melden Groesse >= 2
 *   Datenbyte 1    Rechts Links Runter Hoch Start A C B
 *   Datenbyte 2    R X Y Z L und drei gesetzte Bits
 * Die drei gesetzten Bits im zweiten Datenbyte unterscheiden einen echten
 * Block von Statusdaten. Gedrueckt ist 0, deshalb am Ende invertieren. */
RA_IM_WRAMDATA static int ra_pad_stelle;  /* 0, 7 oder -1 = noch keine gesehen */

RA_IM_WRAM static int ra_pad_block(u8 *oreg, int i)
{
	u8 anschluss = oreg[i*2];
	u8 kennung, d1, d2;

	if((anschluss & 0xf0)!=0xf0 || (anschluss & 0x0f)==0 || (anschluss & 0x0f)>6)
		return -1;
	kennung = oreg[(i+1)*2];
	if((kennung & 0x0f)<2 || (kennung & 0xe0)!=0x00)
		return -1;
	d1 = oreg[(i+2)*2];
	d2 = oreg[(i+3)*2];
	if((d2 & 0x07)!=0x07)
		return -1;
	return ((d1<<8) | d2) ^ 0xffff;
}

/* SF ("ein Befehl laeuft") sperrt hier nicht: In den OREG steht weiterhin das
 * Ergebnis der letzten Abfrage. Gegen halb geschriebene Register schuetzt die
 * Blockpruefung. */
RA_IM_WRAM static int ra_pad_aus_oreg(void)
{
	u8 *oreg = (u8*)0x20100021;
	int bits;

	if(ra_pad_stelle>=0){
		bits = ra_pad_block(oreg, ra_pad_stelle);
		if(bits>=0){
			return bits;
		}
	}

	if(ra_pad_stelle!=0){
		bits = ra_pad_block(oreg, 0);
		if(bits>=0){
			ra_pad_stelle = 0;
			return bits;
		}
	}
	if(ra_pad_stelle!=7){
		bits = ra_pad_block(oreg, 7);
		if(bits>=0){
			ra_pad_stelle = 7;
			return bits;
		}
	}

	return -1;
}


/* SCU: anstehende Interrupts. Schreiben einer 0 loescht das Bit, eine 1 laesst
 * es stehen. Bit 7 ist der SMPC. */
#define SCU_IST         (*(volatile u32*)0x25fe00a4)
#define SCU_IST_SMPC    0x00000080u

/* Ein INTBACK fuer die Firmware selbst, nur fuer die Uhrzeit beim Speichern.
 *
 * Nur, wenn der Baustein frei ist UND dem Spiel kein Ergebnis zusteht: kein
 * Befehl laeuft (SF), kein Teilergebnis wartet auf "weiter" (SR NPE) und kein
 * SMPC-Interrupt steht fuer das Spiel an (SCU IST). Sonst -1. Den Interrupt,
 * den der eigene Befehl ausloest, loescht die Funktion wieder: Seine Routine
 * wuerde sonst Statusdaten als Antwort auf eine eigene Frage lesen. */
static int ra_smpc_intback(int ireg0, int ireg1)
{
	int n;

	if((SF & 0x01) || (SR & 0x20) || (SCU_IST & SCU_IST_SMPC))
		return -1;

	SF = 0x01;
	IREG0 = ireg0;
	IREG1 = ireg1;
	IREG2 = 0xf0;
	COMREG = 0x10;

	for(n=0; n<200000 && (SF & 0x01); n++);
	if(SF & 0x01)
		return -1;
	if(SR & 0x20){                      /* weitere Teile: nicht gewollt */
		IREG0 = 0x40;
		for(n=0; n<100000 && (SR & 0x20); n++);
	}
	SCU_IST = ~SCU_IST_SMPC;
	return 0;
}

RA_IM_WRAMDATA static int ra_selbst_leer;      /* Bilder ohne gefundenen Block */
RA_IM_WRAMDATA static int ra_selbst_offen;     /* eigene Abfrage laeuft */

/* Datum und Uhrzeit, 7 Byte BCD wie SMPC OREG1..7.
 *
 * Zuerst die Uhrzeit des PC: Das Werkzeug schickt sie alle zehn Sekunden, die
 * STM32 legt sie nach Mailbox +0xFD0 und setzt +0xFD7 auf 0xA5. Ohne
 * Werkzeug bleibt die Konsolenuhr. */
#define RA_UHR_PC           ((volatile u8*)0x223F0FD0)
#define RA_UHR_PC_GUELTIG   0xA5

static void ra_uhr_lesen(u8 *zeit)
{
	int i;

	if(RA_UHR_PC[7]==RA_UHR_PC_GUELTIG){
		for(i=0; i<7; i++)
			zeit[i] = RA_UHR_PC[i];
		return;
	}
	memset(zeit, 0, 7);
	if(ra_smpc_intback(0x01, 0x00))
		return;
	for(i=0; i<7; i++)
		zeit[i] = *(u8*)(0x20100021 + 2 + i*2);
}


/* SMPC SYSRES: ganzes System neu starten, zurueck ins SAROO-Menue. */
static void ra_neustart(void)
{
	int n;

	for(;;){
		for(n=0; n<100000 && (SF & 0x01); n++);
		SF = 0x01;
		COMREG = 0x0d;
		for(n=0; n<2000000; n++);
	}
}


/* Wartet auf den Beginn des naechsten V-Blank. Jede Schleife ist auf etwa ein
 * Bild begrenzt: Bei ausgeschalteter Anzeige steht das V-Blank-Bit dauerhaft
 * auf 1. */
#define BILD_SCHLEIFE   20000

static void ra_bild_warten(void)
{
	int n;

	for(n=0; n<BILD_SCHLEIFE && (TVSTAT & 0x0008); n++);
	for(n=0; n<BILD_SCHLEIFE && (TVSTAT & 0x0008)==0; n++);
}


/* ======================================================================== */
/* Speicherstaende                                                            */
/* ======================================================================== */

static void ra_dateiname(char *aus, int max, int slot)
{
	char k[17];
	int i, n = 0;

	for(i=0; i<16 && ra_kennung[i]; i++){
		char c = ra_kennung[i];
		if(c==' ' || c=='/' || c=='\\' || c==':' || c=='*' || c=='?' ||
		   c=='"' || c=='<' || c=='>' || c=='|')
			c = '_';
		k[n++] = c;
	}
	k[n] = 0;
	if(slot>=SLOTS)
		snprintf(aus, max, "/SAROO/rastate_%s_X%d.bin", k, slot-SLOTS+1);
	else
		snprintf(aus, max, "/SAROO/rastate_%s_%d.bin", k, slot+1);
}


static void ra_slots_lesen(void)
{
	char name[48];
	int s;

	for(s=0; s<SLOTS_ALLE; s++){
		ra_dateiname(name, sizeof(name), s);
		memset(&slot_kopf[s], 0, sizeof(ST_KOPF_T));
		if(ra_lesen(name, 0, ST_KOPF, &slot_kopf[s])!=ST_KOPF){
			slot_kopf[s].magic = 0;
			continue;
		}
		if(slot_kopf[s].magic!=ST_MAGIC || slot_kopf[s].version!=ST_VERSION ||
		   memcmp(slot_kopf[s].kennung, ra_kennung, 16)!=0){
			slot_kopf[s].magic = 0;
		}
	}
}


/* Fortlaufende Nummer fuer den naechsten Schnellstand, gesetzt von
 * ra_auftrag() direkt vor ra_speichern(); 0 fuer die festen Slots. */
static u32 ra_speichern_folge;

static int ra_speichern(int slot)
{
	char name[48];
	ST_KOPF_T kopf;
	u32 ofs = ST_KOPF;
	int b, retv;

	ra_dateiname(name, sizeof(name), slot);

	/* Leerer Kopf zuerst: kuerzt die Datei und macht den Slot ungueltig,
	 * bis alles geschrieben ist. */
	memset(&kopf, 0, sizeof(kopf));
	if(write_file(name, -1, ST_KOPF, &kopf)!=ST_KOPF)
		return -1;

	for(b=0; b<ST_BEREICHE; b++){
		const BEREICH *br = &st_bereiche[b];

		if(b==ST_CRAM){
			u16 *cram = (u16*)br->adresse;
			int i;
			for(i=0; i<br->laenge/2; i++)
				cram_puffer[i] = cram[i];
			retv = write_file(name, ofs, br->laenge, cram_puffer);
			if(retv!=br->laenge)
				return -1;
			ofs += br->laenge;
		}else{
			u32 pos;
			for(pos=0; pos<br->laenge; pos+=ST_STUECK){
				int n = br->laenge - pos;
				if(n>ST_STUECK)
					n = ST_STUECK;
				retv = write_file(name, ofs, n, (void*)(br->adresse+pos));
				if(retv!=n)
					return -1;
				ofs += n;
			}
		}
	}

	kopf.magic = ST_MAGIC;
	kopf.version = ST_VERSION;
	memcpy(kopf.kennung, ra_kennung, 16);
	ra_uhr_lesen(kopf.zeit);
	kopf.sp = ra_sp;
	kopf.folge = ra_speichern_folge;
	ra_speichern_folge = 0;
	/* Spielzeit: Bilder seit dem Spielstart, beim Laden aus dem Stand
	 * uebernommen. TVSTAT Bit 0: PAL. */
	kopf.bilder = ra_nurra_takt;
	kopf.pal = (u8)(TVSTAT & 0x0001);
	if(write_file(name, 0, ST_KOPF, &kopf)!=ST_KOPF)
		return -1;

	slot_kopf[slot] = kopf;
	return 0;
}


static u32 ra_st_laenge(void)
{
	u32 summe = ST_KOPF;
	int b;
	for(b=0; b<ST_BEREICHE; b++)
		summe += st_bereiche[b].laenge;
	return summe;
}


/* Laeuft auf dem Firmware-Stapel und kehrt nicht zurueck. */
static void ra_laden_ausfuehren(int slot)
{
	char name[48];
	u32 ofs = ST_KOPF;
	u32 sp = slot_kopf[slot].sp;
	int b;

	ra_dateiname(name, sizeof(name), slot);

	/* Dem PC-Werkzeug melden, bevor irgendetwas ueberschrieben wird. */
	*RA_STATUS_WORT |= RA_ST_GELADEN;

	for(b=0; b<ST_BEREICHE; b++){
		const BEREICH *br = &st_bereiche[b];

		if(b==ST_CRAM){
			u16 *cram = (u16*)br->adresse;
			int i;
			if(ra_lesen(name, ofs, br->laenge, cram_puffer)!=br->laenge)
				goto kaputt;
			for(i=0; i<br->laenge/2; i++)
				cram[i] = cram_puffer[i];
			ofs += br->laenge;
		}else{
			u32 pos;
			for(pos=0; pos<br->laenge; pos+=ST_STUECK){
				int n = br->laenge - pos;
				if(n>ST_STUECK)
					n = ST_STUECK;
				if(ra_lesen(name, ofs, n, (void*)(br->adresse+pos))!=n)
					goto kaputt;
				ofs += n;
			}
		}
	}

	ra_bild_warten();
	ra_kombi_gesperrt = 1;
	ra_nurra_takt = slot_kopf[slot].bilder;     /* Spielzeit laeuft weiter */
	ra_fortsetzen(sp);

kaputt:
	/* Der Speicher ist schon teilweise ueberschrieben, ein Weiterspielen
	 * waere unsauber. Neustart ins SAROO-Menue. */
	ra_neustart();
}


static int ra_laden_pruefen(int slot)
{
	char name[48];
	u8 test[4];

	if(slot_kopf[slot].magic!=ST_MAGIC)
		return -1;
	/* Datei vollstaendig? Das letzte Byte muss lesbar sein, bevor irgendetwas
	 * ueberschrieben wird. */
	ra_dateiname(name, sizeof(name), slot);
	if(ra_lesen(name, ra_st_laenge()-1, 1, test)!=1)
		return -1;
	return 0;
}


/* Slot-Uebersicht fuer das Werkzeug in die Mailbox, siehe RA_SLOTINFO. */
static void ra_slotinfo_ablegen(void)
{
	int s, i;

	ra_slots_lesen();
	for(s=0; s<SLOTS_ALLE; s++){
		volatile u8 *z = RA_SLOTINFO + s*RA_SLOTINFO_EINTRAG;
		const int belegt = (slot_kopf[s].magic==ST_MAGIC);
		const u32 f = belegt ? slot_kopf[s].folge : 0;
		z[0] = (u8)belegt;
		for(i=0; i<7; i++)
			z[1+i] = belegt ? slot_kopf[s].zeit[i] : 0;
		const u32 b = belegt ? slot_kopf[s].bilder : 0;
		z[8]  = (u8)(f>>8);
		z[9]  = (u8)f;
		z[10] = (u8)(b>>24);
		z[11] = (u8)(b>>16);
		z[12] = (u8)(b>>8);
		z[13] = (u8)b;
		z[14] = belegt ? slot_kopf[s].pal : 0;
		z[15] = 0;
	}
}

/* Ringplatz fuer den naechsten Schnellstand und seine Nummer: eins hinter der
 * hoechsten vorhandenen Nummer, Platz = (Nummer-1) mod SLOTS_X. Erwartet
 * frisch gelesene Koepfe (ra_slots_lesen). */
static int ra_x_naechster(u32 *folge)
{
	u32 hoechste = 0;
	int p;

	for(p=0; p<SLOTS_X; p++){
		const ST_KOPF_T *k = &slot_kopf[SLOTS+p];
		if(k->magic==ST_MAGIC && k->folge>hoechste)
			hoechste = k->folge;
	}
	*folge = hoechste + 1;
	return SLOTS + (int)((hoechste) % SLOTS_X);
}

/* Bit 7 des Ergebnisses wechselt bei jeder Antwort. Die STM32 meldet nur
 * Aenderungen weiter; ohne das Wechselbit kaeme zweimal hintereinander
 * dieselbe Antwort nur einmal beim Werkzeug an. */
static u32 ra_antwort_folge;

static void ra_antwort(u32 vorgang, u32 ergebnis)
{
	ra_antwort_folge ^= 0x80;
	*RA_ANTWORT_WORT = ((u32)RA_ANTWORT_KENN<<16) | (vorgang<<8) |
	                   (ergebnis & 0x7f) | ra_antwort_folge;
}

/* Einen Auftrag ausfuehren: Speichern, Laden oder Slots lesen. Laeuft im
 * Nachlauf des V-Blank-OUT mit dem vollstaendigen Registerrahmen des Spiels
 * (ra_sp), alle Interrupts gesperrt.
 *
 * Nichts hier wartet ohne Grenze: Die Dateizugriffe enden mit Fehler, wenn
 * die STM32 nicht antwortet, und jeder Fehler endet im Spiel. Einzige
 * Ausnahme ist ein Laden, das mittendrin scheitert: Dann ist der Speicher
 * schon halb ueberschrieben, und es geht ins SAROO-Menue. */
static void ra_auftrag(u32 auftrag)
{
	const u32 vorgang = (auftrag>>8) & 0xff;
	int slot          = (int)(auftrag & 0xff);
	u32 sr;

	__asm volatile ( "stc\tsr,%0": "=r" (sr) );
	__asm volatile ( "ldc\t%0,sr" :: "r" (sr | 0xF0) );

	/* Neuer Schnellstand: Platz im Ring bestimmen, Nummer mitgeben. */
	if(slot==SLOT_X_NEU && vorgang==RA_VG_SPEICHERN && !ra_gesperrt){
		ra_slots_lesen();
		slot = ra_x_naechster(&ra_speichern_folge);
	}

	if(slot==SLOT_X_NEU && vorgang==RA_VG_SPEICHERN && ra_gesperrt){
		ra_antwort(vorgang, RA_ERG_GESPERRT);
	}else if(slot<0 || slot>=SLOTS_ALLE){
		ra_antwort(vorgang, RA_ERG_UNBEKANNT);
	}else if(vorgang==RA_VG_SLOTS){
		ra_slotinfo_ablegen();
		ra_antwort(vorgang, RA_ERG_OK);
	}else if(ra_gesperrt && (vorgang==RA_VG_SPEICHERN || vorgang==RA_VG_LADEN)){
		ra_antwort(vorgang, RA_ERG_GESPERRT);
	}else if(vorgang==RA_VG_SPEICHERN){
		const int ok = (ra_speichern(slot)==0);
		ra_slotinfo_ablegen();
		ra_antwort(vorgang, ok ? RA_ERG_OK : RA_ERG_FEHLER);
	}else if(vorgang==RA_VG_LADEN){
		ra_slots_lesen();
		if(ra_laden_pruefen(slot)){
			ra_antwort(vorgang, RA_ERG_LEER);
		}else{
			/* Vor dem Laden melden: Danach kehrt nichts mehr hierher zurueck.
			 * Das Work-RAM mit dem aktuellen Stapel wird gleich
			 * ueberschrieben; weiter auf dem Firmware-Stapel. */
			ra_antwort(vorgang, RA_ERG_OK);
			ra_kombi_gesperrt = 1;
			ra_eigener_stapel(ra_laden_ausfuehren, slot);
		}
	}else{
		ra_antwort(vorgang, RA_ERG_UNBEKANNT);
	}

	ra_kombi_gesperrt = 1;
	__asm volatile ( "ldc\t%0,sr" :: "r" (sr) );
}


/* ======================================================================== */
/* Tasten                                                                     */
/* ======================================================================== */

/* Zuletzt gesehener Tastenstand von Pad 1, -1 solange keiner kam.
 *
 * Zwei Stellen fuellen ihn, weil Spiele den SMPC unterschiedlich abfragen:
 *   - nach dem V-Blank-OUT-Handler (ra_ingame_pruefen), wenn das Spiel nur die
 *     Controller anfordert;
 *   - nach dem SMPC-Interrupt (ra_smpc_nachlauf), wenn das Spiel Status und
 *     Controller zusammen anfordert. Dann liegen zum V-Blank-OUT nur die
 *     Statusdaten in den OREG, die Tasten kommen mit einem spaeteren
 *     Interrupt. */
RA_IM_WRAMDATA static int ra_pad_letzt;

RA_IM_WRAM static void ra_pad_merken(void)
{
	int bits = ra_pad_aus_oreg();
	if(bits>=0){
		if(ra_pad_letzt != bits)
			ra_pad_letzt = bits;
		if(ra_selbst_leer)
			ra_selbst_leer = 0;
	}else if(ra_selbst_leer < 30000){
		ra_selbst_leer += 1;
	}

	/* Ergebnis einer eigenen Abfrage ist abgeholt: dem Baustein "fertig"
	 * sagen, damit er nicht auf ein Weiter wartet. */
	if(ra_selbst_offen && !(SF & 0x01)){
		ra_selbst_offen = 0;
		IREG0 = 0x40;
	}
}

/* Nach jedem SCU-Timer-0- bzw. V-Blank-IN-Interrupt, aus den vollstaendigen
 * Vorschaltungen in ra_ingame_s.S. In dieser Fassung werden diese Vektoren
 * nicht uebernommen; die Funktionen bleiben, weil ra_ingame_s.S sie
 * referenziert. */
void ra_timer_nachlauf(void)
{
	ra_watch_schreiben();
}

void ra_vbi_nachlauf(void)
{
	if(!ra_timer_an)
		ra_watch_schreiben();
}

/* Nach jedem SMPC-Interrupt, siehe ra_ingame_s.S. */
void ra_smpc_nachlauf(void)
{
	ra_pad_merken();
}


/* Nach jedem V-Blank-OUT-Handler des Spiels, aus ra_ingame_s.S. Rueckgabe 1
 * laesst ra_ingame_s.S ra_ingame_menue() mit dem vollstaendigen
 * Registerrahmen aufrufen, dort wird der Auftrag ausgefuehrt.
 *
 * Liegt in .wramtext, siehe RA_IM_WRAM. */
RA_IM_WRAM int ra_ingame_pruefen(void)
{
	++ra_nurra_takt;

	/* Erst das Protokoll, dann der Weg ueber die Mailbox. Beide teilen
	 * watch_letzt; weil dazwischen kein Spielcode laeuft, liest der zweite
	 * dieselben Werte und schreibt nur noch in seiner Vollrunde. */
	if(ra_prot_an)
		ra_prot_bild();
	{
		int k;
		for(k=0; k<RA_JE_BILD; k++)
			ra_watch_schreiben();
	}

	/* Auftrag des PC-Werkzeugs, siehe RA_AUFTRAG_WORT. Das Wort wird sofort
	 * geloescht, damit der Auftrag genau einmal laeuft. */
	if((ra_nurra_takt & 7)==0){
		const u32 a = *RA_AUFTRAG_WORT;
		if((a>>16)==RA_AUFTRAG_KENN){
			*RA_AUFTRAG_WORT = 0;
			ra_auftrag_jetzt = a & 0xffff;
			return 1;
		}
	}

	/* L + R + Unten: neuer Schnellstand. Danach erst wieder, wenn die
	 * Kombination einmal losgelassen gemeldet wurde. Im gesperrten Zustand
	 * lehnt ra_auftrag() ab und meldet es dem Werkzeug. */
	ra_pad_merken();
	if(ra_pad_letzt>=0){
		const int kombi = (ra_pad_letzt & PAD_KOMBI_LR)==PAD_KOMBI_LR;
		if(ra_kombi_gesperrt){
			if(!kombi)
				ra_kombi_gesperrt = 0;
		}else if(kombi){
			ra_auftrag_jetzt = (RA_VG_SPEICHERN<<8) | SLOT_X_NEU;
			return 1;
		}
	}
	return 0;
}


/* sp zeigt auf alle gesicherten Register, siehe ra_ingame_s.S. */
void ra_ingame_menue(u32 sp)
{
	ra_sp = sp;
	if(ra_auftrag_jetzt){
		const u32 a = ra_auftrag_jetzt;
		ra_auftrag_jetzt = 0;
		ra_auftrag(a);
		return;
	}
}


/* RA-Hook aus dem Hook-Satz, siehe kHookWorte im Werkzeug (core/setup.cpp).
 * Firmware und Werkzeug muessen dieselbe Fassung tragen. Wort 8 ist das
 * Sprungziel am Ende der Kette. */
#define HOOK_BASIS      0x023F1000
#define HOOK_WORTE      9
#define HOOK_ZIEL       8
static const u32 hook_aktuell[HOOK_WORTE] = {
	0x2f060002, 0x2f06d004, 0x2f06d005, 0x402b50f2,
	0x60f6002b, 0x00090009, HOOK_BASIS + 0x10, 0x223f0000,
	0x06000846,
};


/* ------------------------------------------------------------------------
 * Eigene V-Blank-Routinen eines Spiels
 *
 * Das BIOS kennt zwei Wege, eine Interrupt-Routine anzumelden:
 *   0x06000300/304  SCU-Interrupt setzen/lesen: schreibt nur in die BIOS-
 *                   Tabelle ab 0x06000900; der Vektor bleibt beim BIOS-
 *                   Verteiler, die Kette davor bleibt erhalten.
 *   0x06000310/314  SH-2-Interrupt setzen/lesen: schreibt VBR + Nummer*4,
 *                   also genau den Eintrag, in dem die Vorschaltung haengt.
 * Der SH-2-Weg wird fuer die V-Blank-Vektoren abgefangen: Die Vorschaltung
 * bleibt im Vektor, die Routine des Spiels kommt an das Ende der Kette, und
 * Lesen liefert dem Spiel, was es angemeldet hat. Alles andere geht
 * unveraendert an das BIOS.
 * ------------------------------------------------------------------------ */

#define BIOS_SETSCU     ((u32*)0x06000300)
#define BIOS_SETSH2     ((u32*)0x06000310)
#define BIOS_GETSH2     ((u32*)0x06000314)
#define VEKTOR_VBO      0x41
#define VEKTOR_VBI      0x40

static void  (*bios_setscu)(u32 nummer, void *routine);
static void  (*bios_setsh2)(u32 nummer, void *routine);
static void *(*bios_getsh2)(u32 nummer);
static u32 vbo_grundwert;               /* Ende der Kette beim Spielstart */
static u32 vbi_grundwert;               /* V-Blank-IN-Handler beim Spielstart */
static u32 vbi_spiel;                   /* was das Spiel dort angemeldet glaubt */
static u32 vbo_spiel;                   /* dito fuer V-Blank-OUT */

static u32 ra_vbr(void)
{
	u32 vbr;
	__asm volatile ( "stc\tvbr,%0": "=r" (vbr) );
	return vbr;
}

static int ra_hook_ist_aktuell(void)
{
	u32 *h = (u32*)HOOK_BASIS;
	/* Verglichen werden Code-Worte, keine Literale: Mailbox und Sprungziel
	 * sind je Spiel verschieden. */
	return ra_vbo_next==HOOK_BASIS && h[0]==hook_aktuell[0] &&
	       h[1]==hook_aktuell[1] && h[4]==hook_aktuell[4];
}

static u32 ra_vbo_ende(void)
{
	return ra_hook_ist_aktuell()? ((u32*)HOOK_BASIS)[HOOK_ZIEL] : ra_vbo_next;
}

static void ra_vbo_ende_setzen(u32 ziel)
{
	if(ra_hook_ist_aktuell())
		((u32*)HOOK_BASIS)[HOOK_ZIEL] = ziel;   /* Sprungziel des RA-Hooks */
	else
		ra_vbo_next = ziel;
	ra_vbo_schlank_ziel = ra_vbo_next;          /* Literal im schlanken Einstieg */
}

static void ra_setscu(u32 nummer, void *routine)
{
	bios_setscu(nummer, routine);
}

static void ra_setsh2(u32 nummer, void *routine)
{
	if(nummer==VEKTOR_VBO && ra_vbr()==0x06000000 && *VBO_VEKTOR==RA_VBO_EINSTIEG){
		vbo_spiel = (routine)? (u32)routine : vbo_grundwert;
		ra_vbo_ende_setzen(vbo_spiel);
		return;
	}
	if(nummer==VEKTOR_VBI && ra_vbr()==0x06000000 && *VBI_VEKTOR==(u32)ra_vbi_entry){
		vbi_spiel = (routine)? (u32)routine : vbi_grundwert;
		ra_vbi_next = vbi_spiel;
		return;
	}
	bios_setsh2(nummer, routine);
}

static void *ra_getsh2(u32 nummer)
{
	if(nummer==VEKTOR_VBO && ra_vbr()==0x06000000 && *VBO_VEKTOR==RA_VBO_EINSTIEG)
		return (void*)vbo_spiel;
	if(nummer==VEKTOR_VBI && ra_vbr()==0x06000000 && *VBI_VEKTOR==(u32)ra_vbi_entry)
		return (void*)vbi_spiel;
	return bios_getsh2(nummer);
}


/* Beide VDP1-Bildspeicher vor dem Spielstart leeren.
 *
 * Manche Spiele beschreiben einen Teil des Bildspeichers nie; dort stand
 * sonst, was das SAROO-Menue oder das vorige Spiel hinterlassen hat (etwa ein
 * farbiger Block am rechten Rand in Croc). game_load.c leert bereits Befehls-
 * und Texturspeicher, die beiden Bildspeicher nicht. Der Zugriff geht auf den
 * jeweils verdeckten Puffer bei 0x25C80000; ein Wechsel schaltet den anderen
 * dorthin. */
static void ra_bildspeicher_leeren(void)
{
	volatile u32 *fb = (volatile u32*)0x25C80000;
	volatile u16 *fbcr = (volatile u16*)0x25D00002;
	int durchgang, i;
	volatile int t;

	for(durchgang=0; durchgang<2; durchgang++){
		for(i=0; i<0x40000/4; i++)
			fb[i] = 0;
		*fbcr = 0x0003;             /* Wechsel von Hand anfordern */
		for(t=0; t<200000; t++)     /* auf den Wechsel warten */
			;
	}
}


/* Aus read_1st() in game_load.c, nachdem patch_game() die Konfiguration samt
 * RA-Hook eingetragen hat. */
void ra_ingame_install(void)
{
	int i;

	/* Zuerst die Worte in .wramdata setzen; dort steht, was zuvor dort stand.
	 * ra_w zeigt zunaechst auf den Block in der Kartusche; ob es der im
	 * Block-RAM eines erweiterten FPGA wird, entscheidet die Probe in
	 * ra_watch_laden(). */
	ra_menue_offen = 0;
	ra_w = &ra_w_ersatz;
	ra_nurra_takt = 0;

	ra_bildspeicher_leeren();

	memcpy(ra_kennung, (u8*)0x06002020, 16);
	for(i=16; i>0 && (ra_kennung[i-1]==' ' || ra_kennung[i-1]==0); i--);
	ra_kennung[i] = 0;
	for(; i<17; i++)
		ra_kennung[i] = 0;

	ra_gesperrt = (*RA_STATUS_WORT==RA_WUNSCH_SPERRE);
	*RA_STATUS_WORT = RA_STATUS_KENN | (ra_gesperrt? RA_ST_GESPERRT : 0);

	ra_watch_laden();

	/* Code an seinen Platz bringen, BEVOR ein Vektor darauf zeigt. Klappt das
	 * nicht, haengt sich die Firmware nicht ein: Das Spiel laeuft dann wie
	 * mit der unveraenderten SAROO-Firmware. */
	if(!ra_wram_code_kopieren())
		return;

	/* Schlanker Einstieg ohne Datenzugriffe vor der Routine des Spiels, siehe
	 * _ra_vbo_schlank in ra_ingame_s.S. Sprungziel zuerst, dann der Vektor. */
	ra_vbo_next = *VBO_VEKTOR;
	ra_vbo_schlank_ziel = ra_vbo_next;
	*VBO_VEKTOR = (u32)ra_vbo_schlank;

	ra_pad_letzt = -1;
	ra_kombi_gesperrt = 0;
	ra_auftrag_jetzt = 0;
	ra_pad_stelle = -1;
	ra_selbst_leer = 0;
	ra_selbst_offen = 0;

	/* SMPC-Einstieg fuer die Tasten, siehe ra_pad_letzt. */
	{
		extern void ra_smpc_schlank(void);
		extern u32 ra_smpc_schlank_ziel;
		ra_smpc_next = *SMPC_VEKTOR;
		ra_smpc_schlank_ziel = ra_smpc_next;
		*SMPC_VEKTOR = (u32)ra_smpc_schlank;
	}

	ra_timer_an = 0;

	/* Der V-Blank-IN bleibt beim Spiel. */
	ra_vbi_next = *VBI_VEKTOR;
	vbi_grundwert = ra_vbi_next;
	vbi_spiel = vbi_grundwert;

	vbo_grundwert = ra_vbo_ende();
	vbo_spiel = vbo_grundwert;

	bios_setscu = (void*)*BIOS_SETSCU;
	bios_setsh2 = (void*)*BIOS_SETSH2;
	bios_getsh2 = (void*)*BIOS_GETSH2;
	*BIOS_SETSCU = (u32)ra_setscu;
	*BIOS_SETSH2 = (u32)ra_setsh2;
	*BIOS_GETSH2 = (u32)ra_getsh2;
}
