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

// Gleichstufige Stimmung, A4 = 440 Hz. Halbtoene, weil die Sterbemelodie
// chromatisch faellt - das ist das Stilmittel, das sie als solche erkennbar
// macht.
#define N_C3 131
#define N_D3 147
#define N_Eb3 156
#define N_F3 175
#define N_Gb3 185
#define N_G3 196
#define N_Ab3 208
#define N_Bb3 233
#define N_C4 262
#define N_Db4 277
#define N_D4 294
#define N_Eb4 311
#define N_E4 330
#define N_F4 349
#define N_Gb4 370
#define N_G4 392
#define N_Ab4 415
#define N_A4 440
#define N_Bb4 466
#define N_B4 494
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

// Die Sterbemelodie leiser: sie laeuft ueber die Game-Over-Blende und in die
// naechste Partie hinein, soll dabei aber nicht anschreien.
#define A_DEAD_LEAD 90
#define A_DEAD_BASS 70
#define A_DEAD_TOLL 45

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


/* --------------------------------------------------------- Sterbemelodie */
/*
   Rund 10 Sekunden, nach den ueblichen Mitteln des Genres gebaut:

     1. Der Sturz      chromatisch fallend, schnell - der Boden gibt nach
     2. Die Klage      Moll, schrittweise abwaerts, Noten werden laenger
     3. Ritardando     jede Note laenger als die vorige, das Stueck kommt
                       zum Stehen
     4. Der Schluss    tiefer Grundton, lang gehalten

   Die Mittel sind Konvention und niemandes Eigentum; die Noten sind eigene.
   Eine bekannte Sterbemelodie abzuschreiben waere dagegen eine Uebernahme -
   die sind urheberrechtlich geschuetzt, so kurz sie auch sind.
*/

static const SoundNote MUSIC_DEAD_LEAD[] = {
    // 1. Der Sturz - chromatisch von C5 hinunter nach C4
    {N_C5, 100, A_DEAD_LEAD}, {N_B4,  100, A_DEAD_LEAD}, {N_Bb4, 100, A_DEAD_LEAD},
    {N_A4, 100, A_DEAD_LEAD}, {N_Ab4, 100, A_DEAD_LEAD}, {N_G4,  100, A_DEAD_LEAD},
    {N_Gb4,100, A_DEAD_LEAD}, {N_F4,  100, A_DEAD_LEAD}, {N_E4,  100, A_DEAD_LEAD},
    {N_Eb4,100, A_DEAD_LEAD}, {N_D4,  100, A_DEAD_LEAD}, {N_Db4, 100, A_DEAD_LEAD},
    {N_C4, 200, A_DEAD_LEAD},

    // 2. Die Klage - c-Moll, Noten werden laenger
    {N_Eb4, 400, A_DEAD_LEAD}, {N_D4, 400, A_DEAD_LEAD}, {N_C4, 400, A_DEAD_LEAD},
    {N_Bb3, 400, A_DEAD_LEAD}, {N_C4, 600, A_DEAD_LEAD}, {N_Ab3, 600, A_DEAD_LEAD},
    {N_G3, 800, A_DEAD_LEAD},

    // 3. Ritardando - jede Note laenger als die vorige
    {N_Ab3, 500, A_DEAD_LEAD}, {N_G3, 600, A_DEAD_LEAD},
    {N_Gb3, 700, A_DEAD_LEAD}, {N_F3, 800, A_DEAD_LEAD},

    // 4. Der Schluss
    {0, 200, 0}, {N_Eb3, 300, A_DEAD_LEAD}, {N_D3, 400, A_DEAD_LEAD},
    {N_C3, 1500, A_DEAD_LEAD},
};

// Liegende Basstoene, die dem Fall den Boden geben
static const SoundNote MUSIC_DEAD_BASS[] = {
    {N_C3, 1400, A_DEAD_BASS}, {N_Ab3, 1800, A_DEAD_BASS}, {N_G3, 1800, A_DEAD_BASS},
    {N_F3, 1800, A_DEAD_BASS}, {N_Eb3, 1600, A_DEAD_BASS}, {N_C3, 1600, A_DEAD_BASS},
};

// Setzt erst in der zweiten Haelfte ein: zwei fallende Halbtoene, das
// "Wah-wah" der Verlierer-Kadenz, dann ein langer Ton.
static const SoundNote MUSIC_DEAD_TOLL[] = {
    {0, 5000, 0},
    {N_Eb4, 600, A_DEAD_TOLL}, {N_D4, 600, A_DEAD_TOLL},
    {0, 200, 0},
    {N_Db4, 800, A_DEAD_TOLL}, {N_C4, 2800, A_DEAD_TOLL},
};

static const SoundMusic MUSIC_DEAD = {
    {
        {NULL, 0},
        {MUSIC_DEAD_LEAD, sizeof(MUSIC_DEAD_LEAD) / sizeof(SoundNote)},
        {MUSIC_DEAD_BASS, sizeof(MUSIC_DEAD_BASS) / sizeof(SoundNote)},
        {MUSIC_DEAD_TOLL, sizeof(MUSIC_DEAD_TOLL) / sizeof(SoundNote)},
    },
    false,
};

#endif
