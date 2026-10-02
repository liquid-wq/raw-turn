! Vorschaltungen vor den Interrupt-Routinen des Spiels.
!
! Die Arbeit laeuft NACH der Routine des Spiels, nicht davor: Zusaetzliche
! Rechenzeit vor der V-Blank-OUT-Routine kann Spiele stoeren, die dort den
! Bildspeicher wechseln.
!
! Einstieg: r0 sichern, darunter einen kuenstlichen Ausnahme-Rahmen legen
! (aktuelles SR, Ruecksprung in den Nachlauf) und mit unveraenderten Registern
! zum bisherigen Handler springen. Dessen rte landet im Nachlauf.
!
! Nachlauf: Die Register, die eine C-Funktion veraendern darf, kommen auf den
! Stapel, dann laeuft ra_ingame_pruefen(). Liefert sie 1, kommen r8-r14 und
! GBR dazu. Damit liegen ALLE Register des unterbrochenen Spiels samt seinem
! Ausnahme-Rahmen auf dessen Stapel, und der Stapelzeiger geht an
! ra_ingame_menue(sp). Der Stapel liegt im Work-RAM und ist Teil jedes
! Speicherstands: Wer ihn zurueckschreibt und den Stapelzeiger von damals
! setzt, kehrt ueber denselben Rueckweg exakt in den gesicherten Zustand
! zurueck (ra_fortsetzen).
!
! Der Block liegt in .wramtext, siehe ldscript_kartusche und
! ra_wram_code_kopieren. Alle Spruenge sind PC-relativ, alle absoluten
! Adressen stehen als Literale im Block; er laesst sich deshalb auch an eine
! andere Adresse kopieren.
    .section .wramtext
    .align  2
    .global _ra_vbo_entry
    .global _ra_fortsetzen
    .global _ra_eigener_stapel
    .global _ra_smpc_entry

! SMPC-Interrupt (Vektor 0x47): gleicher Aufbau, im Nachlauf nur
! ra_smpc_nachlauf(), das die Tasten aus den OREG uebernimmt.
_ra_smpc_entry:
    mov.l   r0,@-r15
    stc     sr,r0
    mov.l   r0,@-r15
    mov.l   .Lsmpc_post,r0
    mov.l   r0,@-r15
    mov.l   .Lsmpc_next,r0
    mov.l   @r0,r0
    jmp     @r0
    mov.l   @(8,r15),r0

.Lsmpc_nach:
    mov.l   r1,@-r15
    mov.l   r2,@-r15
    mov.l   r3,@-r15
    mov.l   r4,@-r15
    mov.l   r5,@-r15
    mov.l   r6,@-r15
    mov.l   r7,@-r15
    sts.l   mach,@-r15
    sts.l   macl,@-r15
    sts.l   pr,@-r15
    mov.l   .Lsmpc_func,r0
    jsr     @r0
    nop
    lds.l   @r15+,pr
    lds.l   @r15+,macl
    lds.l   @r15+,mach
    mov.l   @r15+,r7
    mov.l   @r15+,r6
    mov.l   @r15+,r5
    mov.l   @r15+,r4
    mov.l   @r15+,r3
    mov.l   @r15+,r2
    mov.l   @r15+,r1
    mov.l   @r15+,r0
    rte
    nop

    .align  2
.Lsmpc_post:
    .long   .Lsmpc_nach
.Lsmpc_next:
    .long   _ra_smpc_next
.Lsmpc_func:
    .long   _ra_smpc_nachlauf

! V-Blank-IN-Sperre (Vektor 0x40): Ruft die Routine des Spiels nur auf,
! solange ra_menue_offen 0 ist. Ohne Nachlauf.
    .global _ra_vbi_gate
_ra_vbi_gate:
    mov.l   r0,@-r15
    mov.l   .Lgate_offen,r0
    mov.l   @r0,r0
    tst     r0,r0
    bf      .Lgate_zu

    mov.l   .Lgate_next,r0
    mov.l   @r0,r0
    jmp     @r0                 ! weiter zur Routine des Spiels
    mov.l   @r15+,r0            ! Delay-Slot: r0 des Spiels zurueck

.Lgate_zu:
    mov.l   @r15+,r0
    rte
    nop

    .align  2
.Lgate_offen:
    .long   _ra_menue_offen
.Lgate_next:
    .long   _ra_vbi_next

! V-Blank-IN (Vektor 0x40): gleicher Aufbau, im Nachlauf ra_vbi_nachlauf().
    .global _ra_vbi_entry
_ra_vbi_entry:
    mov.l   r0,@-r15
    stc     sr,r0
    mov.l   r0,@-r15
    mov.l   .Lvbi_post,r0
    mov.l   r0,@-r15
    mov.l   .Lvbi_next,r0
    mov.l   @r0,r0
    jmp     @r0
    mov.l   @(8,r15),r0

.Lvbi_nach:
    mov.l   r1,@-r15
    mov.l   r2,@-r15
    mov.l   r3,@-r15
    mov.l   r4,@-r15
    mov.l   r5,@-r15
    mov.l   r6,@-r15
    mov.l   r7,@-r15
    sts.l   mach,@-r15
    sts.l   macl,@-r15
    sts.l   pr,@-r15
    mov.l   .Lvbi_func,r0
    jsr     @r0
    nop
    lds.l   @r15+,pr
    lds.l   @r15+,macl
    lds.l   @r15+,mach
    mov.l   @r15+,r7
    mov.l   @r15+,r6
    mov.l   @r15+,r5
    mov.l   @r15+,r4
    mov.l   @r15+,r3
    mov.l   @r15+,r2
    mov.l   @r15+,r1
    mov.l   @r15+,r0
    rte
    nop

    .align  2
.Lvbi_post:
    .long   .Lvbi_nach
.Lvbi_next:
    .long   _ra_vbi_next
.Lvbi_func:
    .long   _ra_vbi_nachlauf

! SCU-Timer-0-Interrupt (Vektor 0x43): gleicher Aufbau wie SMPC.
! _ra_timer_rte ist ein leerer Abschluss der Kette, falls im Vektor keine
! brauchbare Routine steht.
    .global _ra_timer_rte
_ra_timer_rte:
    rts
    nop

    .global _ra_timer_entry
_ra_timer_entry:
    mov.l   r0,@-r15
    stc     sr,r0
    mov.l   r0,@-r15
    mov.l   .Ltim_post,r0
    mov.l   r0,@-r15
    mov.l   .Ltim_next,r0
    mov.l   @r0,r0
    jmp     @r0
    mov.l   @(8,r15),r0

.Ltim_nach:
    mov.l   r1,@-r15
    mov.l   r2,@-r15
    mov.l   r3,@-r15
    mov.l   r4,@-r15
    mov.l   r5,@-r15
    mov.l   r6,@-r15
    mov.l   r7,@-r15
    sts.l   mach,@-r15
    sts.l   macl,@-r15
    sts.l   pr,@-r15
    mov.l   .Ltim_func,r0
    jsr     @r0
    nop
    lds.l   @r15+,pr
    lds.l   @r15+,macl
    lds.l   @r15+,mach
    mov.l   @r15+,r7
    mov.l   @r15+,r6
    mov.l   @r15+,r5
    mov.l   @r15+,r4
    mov.l   @r15+,r3
    mov.l   @r15+,r2
    mov.l   @r15+,r1
    mov.l   @r15+,r0
    rte
    nop

    .align  2
.Ltim_post:
    .long   .Ltim_nach
.Ltim_next:
    .long   _ra_timer_next
.Ltim_func:
    .long   _ra_timer_nachlauf

! Vollstaendige V-Blank-OUT-Vorschaltung (Vektor 0x41). Ist ra_menue_offen
! gesetzt, wird die Routine des Spiels nicht aufgerufen und kein Nachlauf
! ausgefuehrt: Das Spiel steht still.
    .align  2
_ra_vbo_entry:
    mov.l   r0,@-r15

    mov.l   .Lmoffen,r0
    mov.l   @r0,r0
    tst     r0,r0
    bf      .Lmenue_laeuft

    stc     sr,r0
    mov.l   r0,@-r15
    mov.l   .Lpost,r0
    mov.l   r0,@-r15
    mov.l   .Lnext,r0
    mov.l   @r0,r0
    jmp     @r0
    mov.l   @(8,r15),r0         ! Delay-Slot: r0 des Spiels zurueck

.Lmenue_laeuft:
    mov.l   @r15+,r0
    rte
    nop

.Lnach:
    mov.l   r1,@-r15
    mov.l   r2,@-r15
    mov.l   r3,@-r15
    mov.l   r4,@-r15
    mov.l   r5,@-r15
    mov.l   r6,@-r15
    mov.l   r7,@-r15
    sts.l   mach,@-r15
    sts.l   macl,@-r15
    sts.l   pr,@-r15

    mov.l   .Lpruef,r0
    jsr     @r0
    nop
    tst     r0,r0
    bt      .Lraus

    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   r14,@-r15
    stc.l   gbr,@-r15

    mov.l   .Lmenue,r0
    jsr     @r0
    mov     r15,r4              ! Delay-Slot: sp als erstes Argument

.Lrueckweg:
    ldc.l   @r15+,gbr
    mov.l   @r15+,r14
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    mov.l   @r15+,r8

.Lraus:
    lds.l   @r15+,pr
    lds.l   @r15+,macl
    lds.l   @r15+,mach
    mov.l   @r15+,r7
    mov.l   @r15+,r6
    mov.l   @r15+,r5
    mov.l   @r15+,r4
    mov.l   @r15+,r3
    mov.l   @r15+,r2
    mov.l   @r15+,r1
    mov.l   @r15+,r0            ! r0 aus dem Einstieg
    rte
    nop

! Schlanker V-Blank-OUT-Einstieg, der im Betrieb benutzte.
!
! Vor der Routine des Spiels laeuft hier nur das Noetigste: kein Lesen von
! ra_menue_offen, und das Sprungziel steht als Literal direkt neben dem Code
! statt in einer Variablen. Der Nachlauf ist derselbe wie oben.
!
! _ra_vbo_schlank_ziel wird beim Einhaengen gesetzt (ra_ingame_install).
    .align  2
    .global _ra_vbo_schlank
    .global _ra_vbo_schlank_ziel
_ra_vbo_schlank:
    mov.l   r0,@-r15
    stc     sr,r0
    mov.l   r0,@-r15
    mov.l   .Lsl_post,r0
    mov.l   r0,@-r15
    mov.l   _ra_vbo_schlank_ziel,r0
    jmp     @r0
    mov.l   @(8,r15),r0         ! Delay-Slot: r0 des Spiels zurueck
    .align  2
.Lsl_post:
    .long   .Lnach
_ra_vbo_schlank_ziel:
    .long   0

! Schlanker SMPC-Einstieg, gleiches Prinzip.
!
! Manche Spiele fragen Status und Tasten in zwei Schritten ab; zum V-Blank-OUT
! stehen in den OREG dann nur die Statusdaten. Die Tasten kommen mit einem
! spaeteren SMPC-Interrupt, der Nachlauf .Lsmpc_nach liest sie.
    .align  2
    .global _ra_smpc_schlank
    .global _ra_smpc_schlank_ziel
_ra_smpc_schlank:
    mov.l   r0,@-r15
    stc     sr,r0
    mov.l   r0,@-r15
    mov.l   .Lsms_post,r0
    mov.l   r0,@-r15
    mov.l   _ra_smpc_schlank_ziel,r0
    jmp     @r0
    mov.l   @(8,r15),r0         ! Delay-Slot: r0 des Spiels zurueck
    .align  2
.Lsms_post:
    .long   .Lsmpc_nach
_ra_smpc_schlank_ziel:
    .long   0

! void ra_fortsetzen(u32 sp) -- kehrt nicht zurueck
_ra_fortsetzen:
    mov.l   .Lrueck,r0
    jmp     @r0
    mov     r4,r15

! void ra_eigener_stapel(void (*f)(int), int arg) -- kehrt nicht zurueck.
! Ruft f(arg) auf dem Stapel der Firmware auf; der Stapel des Spiels liegt im
! Work-RAM, das beim Laden ueberschrieben wird.
_ra_eigener_stapel:
    mov.l   .Lstapel,r15
    jmp     @r4
    mov     r5,r4

    .align  2
.Lpost:
    .long   .Lnach
.Lpruef:
    .long   _ra_ingame_pruefen
.Lmenue:
    .long   _ra_ingame_menue
.Lnext:
    .long   _ra_vbo_next
.Lmoffen:
    .long   _ra_menue_offen
.Lrueck:
    .long   .Lrueckweg
.Lstapel:
    .long   __stack_end
