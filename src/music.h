/*
    Musikstücke als Notenlisten.

    Eigenkomposition, bewusst: freie Chiptune-Dateien im Netz sind mehrkanalige
    Audioaufnahmen und liessen sich ohnehin nicht direkt einbinden — man müsste
    die Melodie abschreiben. Acht Takte Rechteckwelle selbst zu setzen ist wenig
    Aufwand, und für ein Gerät, das mit eurem Logo an öffentlichen Anlässen
    steht, entfällt damit jede Lizenzfrage.

    Kosten: 6 Bytes pro Note. Das ganze Stück unten sind rund 200 Bytes.

    Stimme 0 bleibt frei — die gehört den Spielgeräuschen.
*/
#ifndef MUSIC_H
#define MUSIC_H

#include "sound.h"

// Gleichstufige Stimmung, A4 = 440 Hz
#define N_C3 131
#define N_F3 175
#define N_G3 196
#define N_C4 262
#define N_E4 330
#define N_G4 392
#define N_C5 523
#define N_E5 659
#define N_G5 784
#define N_C6 1047

// Lautstärken so gewählt, dass die Summe der drei Stimmen unter der
// Begrenzung des Mischers bleibt: (110 + 80 + 40) / 2 = 115 von 127.
// Höhere Werte klängen nicht lauter, nur verzerrter.
#define A_LEAD 110
#define A_BASS 80
#define A_ARP 40

/* ---------------------------------------------------------- Intro-Fanfare */
/* Rund 2,4 Sekunden, aufsteigend, endet auf dem hohen Grundton. */

static const SoundNote MUSIC_INTRO_LEAD[] = {
    {N_G4, 240, A_LEAD}, {N_C5, 240, A_LEAD}, {N_E5, 240, A_LEAD}, {N_G5, 480, A_LEAD},
    {0,    120, 0},      {N_E5, 240, A_LEAD}, {N_G5, 240, A_LEAD}, {N_C6, 600, A_LEAD},
};

static const SoundNote MUSIC_INTRO_BASS[] = {
    {N_C3, 480, A_BASS}, {N_G3, 480, A_BASS}, {N_C3, 480, A_BASS},
    {N_F3, 480, A_BASS}, {N_G3, 480, A_BASS},
};

// Schnelle Begleitfigur, füllt die Fläche zwischen Melodie und Bass.
static const SoundNote MUSIC_INTRO_ARP[] = {
    {N_C4, 120, A_ARP}, {N_E4, 120, A_ARP}, {N_G4, 120, A_ARP}, {N_C5, 120, A_ARP},
    {N_C4, 120, A_ARP}, {N_E4, 120, A_ARP}, {N_G4, 120, A_ARP}, {N_C5, 120, A_ARP},
    {N_C4, 120, A_ARP}, {N_E4, 120, A_ARP}, {N_G4, 120, A_ARP}, {N_C5, 120, A_ARP},
    {N_C4, 120, A_ARP}, {N_E4, 120, A_ARP}, {N_G4, 120, A_ARP}, {N_C5, 120, A_ARP},
    {N_C4, 120, A_ARP}, {N_E4, 120, A_ARP}, {N_G4, 120, A_ARP}, {N_C5, 120, A_ARP},
};

static const SoundMusic MUSIC_INTRO = {
    {
        {NULL, 0},                                                      // 0: Spielgeräusche
        {MUSIC_INTRO_LEAD, sizeof(MUSIC_INTRO_LEAD) / sizeof(SoundNote)},
        {MUSIC_INTRO_BASS, sizeof(MUSIC_INTRO_BASS) / sizeof(SoundNote)},
        {MUSIC_INTRO_ARP,  sizeof(MUSIC_INTRO_ARP) / sizeof(SoundNote)},
    },
    false, // einmal, nicht in Schleife
};

#endif
