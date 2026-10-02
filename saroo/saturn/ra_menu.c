/*
 * RetroAchievements-Eintrag im SAROO-Startmenue.
 *
 * Listet die Hook-Saetze, die die Kartusche selbst auf der SD-Karte ablegt
 * (/SAROO/rahook_<Spielkennung>.txt), und loescht einzelne davon.
 *
 * Loeschen heisst: Datei auf 0 Byte kuerzen. Das geht ueber den vorhandenen
 * Schreibbefehl der STM32-Firmware. Eine leere Datei traegt beim Spielstart
 * keine Zeile ein und erscheint in dieser Liste nicht mehr. Laeuft das Spiel
 * danach mit verbundenem PC-Werkzeug, legt es den Hook-Satz neu an.
 *
 * Die erste Zeile eines Hook-Satzes darf "; <Spieltitel>" sein. Die Firmware
 * ueberspringt alles, was nicht mit M_ beginnt; hier wird daraus der
 * Listeneintrag.
 */

#include "main.h"

#define RA_LISTE_ARG    0x5241      /* SSCMD_LISTBINS: /SAROO statt /SAROO/BIN */
#define RA_PRAEFIX      "rahook_"
#define RA_PRAEFIX_LEN  7
#define RA_MAX          96
#define RA_TITEL        40

/* Baukennung aus ra_bau.h, die der Bau anlegt, in der Fusszeile des
 * Eintrags. Daran ist am Geraet zu sehen, welches Abbild laeuft. */
#include "ra_bau.h"
#ifndef RA_BAUKENNUNG
#define RA_BAUKENNUNG   "ohne"
#endif

static MENU_DESC ra_menu;
static short ra_idx[RA_MAX];            /* Index in der Dateiliste (IMGINFO) */
static char  ra_titel[RA_MAX][RA_TITEL];
static int   ra_anzahl, ra_seite, ra_seiten;
static int   ra_loeschen = -1;          /* vorgemerkter Eintrag, wartet auf zweites A */


static char *tx(char *en, char *de)
{
	return (lang_id==7)? de : en;
}


static char *ra_dateiname(int k)
{
	int *pfade = (int*)(IMGINFO_ADDR+0x04);
	return (char*)IMGINFO_ADDR + LE32(&pfade[ra_idx[k]]);
}


/* Liefert 0 fuer eine leere (geloeschte) oder unlesbare Datei. */
static int ra_titel_lesen(char *name, char *titel)
{
	char pfad[48], buf[64];
	char *u, *p;
	int n, i;

	snprintf(pfad, sizeof(pfad), "/SAROO/%s", name);
	n = read_file(pfad, 0, sizeof(buf)-1, buf);
	if(n<=0)
		return 0;

	/* Ueber die Adresse ohne Cache lesen: Der Puffer liegt auf dem Stapel der
	 * Firmware in der Kartusche, und die STM32 schreibt fuer read_file()
	 * direkt ins SDRAM. Ueber den Cache saehe die Saturn alte Daten. */
	u = (char*)((u32)buf | 0x20000000);
	u[n] = 0;

	titel[0] = 0;
	if(u[0]!=';')
		return 1;

	p = u+1;
	while(*p==' ')
		p++;
	for(i=0; i<RA_TITEL-1 && p[i] && p[i]!='\r' && p[i]!='\n'; i++)
		titel[i] = p[i];
	/* nicht mitten in einem UTF-8-Zeichen abschneiden */
	while(i>0 && (((u8)p[i])&0xc0)==0x80)
		i--;
	titel[i] = 0;
	return 1;
}


static void ra_liste_laden(void)
{
	int *pfade = (int*)(IMGINFO_ADDR+0x04);
	int gesamt, i;

	SS_ARG = RA_LISTE_ARG;
	SS_CMD = SSCMD_LISTBINS;
	while(SS_CMD);

	ra_anzahl = 0;
	gesamt = LE32((u8*)IMGINFO_ADDR);
	for(i=0; i<gesamt && ra_anzahl<RA_MAX; i++){
		char *name = (char*)IMGINFO_ADDR + LE32(&pfade[i]);
		if(strncmp(name, RA_PRAEFIX, RA_PRAEFIX_LEN))
			continue;
		if(ra_titel_lesen(name, ra_titel[ra_anzahl])==0)
			continue;
		ra_idx[ra_anzahl] = i;
		ra_anzahl += 1;
	}

	ra_seiten = (ra_anzahl+MENU_ITEMS-1)/MENU_ITEMS;
	if(ra_seite>=ra_seiten)
		ra_seite = (ra_seiten)? ra_seiten-1 : 0;
}


static void ra_seite_fuellen(void)
{
	char zeile[96], kennung[32];
	int i, n;

	ra_menu.num = 0;
	for(i=0; i<MENU_ITEMS; i++){
		int k = ra_seite*MENU_ITEMS + i;
		if(k>=ra_anzahl)
			break;

		/* rahook_T-10001G__V1.000.txt -> T-10001G  V1.000 */
		char *name = ra_dateiname(k) + RA_PRAEFIX_LEN;
		for(n=0; n<(int)sizeof(kennung)-1 && name[n] && strcmp(name+n, ".txt"); n++)
			kennung[n] = (name[n]=='_')? ' ' : name[n];
		kennung[n] = 0;

		if(ra_titel[k][0])
			snprintf(zeile, sizeof(zeile), "%s  [%s]", ra_titel[k], kennung);
		else
			snprintf(zeile, sizeof(zeile), "%s", kennung);
		add_menu_item(&ra_menu, zeile);
	}

	if(ra_anzahl==0)
		add_menu_item(&ra_menu, tx("(no hook sets on the card)", "(keine Hook-Sätze auf der Karte)"));

	snprintf(ra_menu.title, sizeof(ra_menu.title), "RetroAchievements Hooks (%d/%d)",
	         (ra_seiten)? ra_seite+1 : 0, ra_seiten);

	if(ra_menu.current>=ra_menu.num)
		ra_menu.current = ra_menu.num-1;
}


static void ra_seite_neu(int ans_ende)
{
	ra_menu.current = 0;
	ra_seite_fuellen();
	ra_menu.current = (ans_ende)? ra_menu.num-1 : 0;
	draw_menu_frame(&ra_menu);
}


static int ra_handle(int ctrl)
{
	MENU_DESC *m = &ra_menu;
	int k = ra_seite*MENU_ITEMS + m->current;

	if(BUTTON_DOWN(ctrl, PAD_A)){
		char pfad[48], leer[4];

		if(ra_anzahl==0)
			return 0;
		if(ra_loeschen!=k){
			ra_loeschen = k;
			menu_status(m, tx("Press A again to delete", "A nochmal: löschen"));
			return 0;
		}
		ra_loeschen = -1;

		snprintf(pfad, sizeof(pfad), "/SAROO/%s", ra_dateiname(k));
		if(write_file(pfad, -1, 0, leer)<0){
			menu_status(m, tx("Delete failed!", "Löschen fehlgeschlagen!"));
			return 0;
		}

		ra_liste_laden();
		ra_seite_fuellen();
		draw_menu_frame(m);
		menu_status(m, tx("Deleted.", "Gelöscht."));
		return 0;
	}

	/* Loslassen einer Taste meldet sich ebenfalls; nur echte Tastendruecke
	 * heben die Vormerkung auf. */
	if(BUTTON_DOWN(ctrl, PAD_UP)){
		ra_loeschen = -1;
		if(m->current>0){
			menu_update(m, m->current-1);
		}else if(ra_seiten>1){
			ra_seite = (ra_seite>0)? ra_seite-1 : ra_seiten-1;
			ra_seite_neu(1);
		}
	}else if(BUTTON_DOWN(ctrl, PAD_DOWN)){
		ra_loeschen = -1;
		if(m->current<(m->num-1)){
			menu_update(m, m->current+1);
		}else if(ra_seiten>1){
			ra_seite = (ra_seite+1<ra_seiten)? ra_seite+1 : 0;
			ra_seite_neu(0);
		}
	}else if(BUTTON_DOWN(ctrl, PAD_LT)){
		ra_loeschen = -1;
		if(ra_seiten>1){
			ra_seite = (ra_seite>0)? ra_seite-1 : ra_seiten-1;
			ra_seite_neu(0);
		}
	}else if(BUTTON_DOWN(ctrl, PAD_RT)){
		ra_loeschen = -1;
		if(ra_seiten>1){
			ra_seite = (ra_seite+1<ra_seiten)? ra_seite+1 : 0;
			ra_seite_neu(0);
		}
	}else if(BUTTON_DOWN(ctrl, PAD_C) || BUTTON_DOWN(ctrl, PAD_B)){
		return MENU_EXIT;
	}

	return 0;
}


void ra_menu_run(void)
{
	memset(&ra_menu, 0, sizeof(ra_menu));
	ra_menu.handle = ra_handle;
	ra_menu.version = tx("b" RA_BAUKENNUNG "  A: delete  C: back",
	                     "b" RA_BAUKENNUNG "  A: löschen  C: zurück");
	ra_seite = 0;
	ra_loeschen = -1;

	ra_liste_laden();
	ra_seite_fuellen();
	menu_run(&ra_menu);

	/* Die Dateiliste liegt im selben Speicher wie die Spieleliste. Wieder
	 * herstellen, wie select_bins() es tut. */
	SS_ARG = 0;
	SS_CMD = SSCMD_LISTDISC;
	while(SS_CMD);
}
