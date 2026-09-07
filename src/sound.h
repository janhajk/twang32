/*
    Mehrstimmige Tonerzeugung über den internen DAC des ESP32, gespeist per
    I2S mit DMA.

    Vorher lief hier ein 1-Bit-Beeper: ein Hardware-Timer schaltete den DAC in
    einem Interrupt zwischen zwei Pegeln um. Das hatte drei Nachteile, und alle
    drei verschwinden mit diesem Aufbau.

    1. EINE Stimme. Musik und Spielgeräusche konnten sich nicht überlagern —
       der Bewegungston hätte jede Melodie zerhackt.
    2. EIN Bit Auflösung. „8-Bit-Musik" meint die Klangchips der 8-Bit-Ära
       (mehrere Pulskanäle, Rauschen, Hüllkurven), nicht die Auflösung. Ein
       Zweipegel-Rechteck ist ZX-Spectrum-Beeper, nicht Game Boy.
    3. `dacWrite()` aus einem Interrupt heraus. Die Funktion liegt im Flash;
       läuft gleichzeitig eine Flash-Operation, stürzt das Gerät ab. Genau
       deshalb musste upstream `sound_pause()` um jedes EEPROM-Schreiben legen.

    Jetzt rechnet ein Task Abtastwerte in einen DMA-Puffer, die Hardware gibt
    sie selbstständig aus. Kein Interrupt im Audiopfad, vier Stimmen, volle
    8 Bit.

    Stimme 0 gehört den Spielgeräuschen (`sound()`), 1 bis 3 der Musik — so
    stört das eine das andere nicht.

    Der DAC-Kanal ist nicht frei wählbar: I2S_DAC_CHANNEL_RIGHT_EN ist fest
    GPIO 25, und genau dort hängt der Verstärker.
*/
#ifndef SOUND_H
#define SOUND_H

#include <driver/i2s.h>

#define DAC_AUDIO_PIN 25 // durch I2S_DAC_CHANNEL_RIGHT_EN festgelegt

#define SOUND_RATE 22050 // Abtastrate; reicht bis ~11 kHz Nutzsignal
#define SOUND_VOICES 4
#define SOUND_SFX_VOICE 0 // für sound() — die Musik fasst diese Stimme nie an

// 4 Puffer à 128 Bilder ≈ 23 ms Vorlauf. Der Kompromiss ist bewusst: mehr
// Puffer überstünde längere Aussetzer im Scheduling, aber eine Änderung wirkt
// erst nach dem Puffer — bei einem Spiel hört man das als träge Treffer.
#define SOUND_DMA_COUNT 4
#define SOUND_DMA_LEN 128

#define MIN_FREQ 20
#define MAX_FREQ 10000

enum SoundWave
{
    WAVE_PULSE = 0, // Rechteck mit einstellbarem Tastverhältnis
    WAVE_NOISE = 1, // Schieberegister-Rauschen, für Perkussion
};

struct SoundVoice
{
    uint32_t phase;  // 0..2^32, oberste 8 Bit = Position in der Periode
    uint32_t step;   // Zuwachs pro Abtastwert
    uint8_t amp;     // 0 = stumm
    uint8_t duty;    // Schwelle 0..255; 128 = symmetrisches Rechteck
    uint8_t wave;
};

struct SoundNote
{
    uint16_t freq; // 0 = Pause
    uint16_t ms;
    uint8_t amp;
};

struct SoundTrack
{
    const SoundNote *notes;
    uint16_t count;
};

/** Musikstück: je eine Notenliste pro Stimme. Stimme 0 bleibt leer. */
struct SoundMusic
{
    SoundTrack voice[SOUND_VOICES];
    bool loop;
};

/* ------------------------------------------------------------------ Zustand */

static SoundVoice sndVoice[SOUND_VOICES];
static volatile bool sndMuted = false;

static const SoundMusic *sndMusic = NULL;
static uint16_t sndSeqIndex[SOUND_VOICES];
static int32_t sndSeqLeft[SOUND_VOICES]; // verbleibende Abtastwerte der Note
static volatile bool sndMusicDone = true;

static uint16_t sndLfsr = 0x7FFF;
static TaskHandle_t sndTaskHandle = NULL;

static inline uint32_t sndStepFor(uint16_t freq)
{
    // 2^32 / Abtastrate, in 64 Bit gerechnet, damit nichts überläuft
    return (uint32_t)(((uint64_t)freq << 32) / SOUND_RATE);
}

/* ---------------------------------------------------------------- Schnittstelle */

/** Ein Spielgeräusch. Belegt ausschliesslich Stimme 0. */
bool sound(uint16_t freq, uint8_t volume)
{
    if (volume == 0 || freq < MIN_FREQ || freq > MAX_FREQ)
    {
        sndVoice[SOUND_SFX_VOICE].amp = 0;
        return false;
    }
    sndVoice[SOUND_SFX_VOICE].step = sndStepFor(freq);
    sndVoice[SOUND_SFX_VOICE].amp = volume;
    sndVoice[SOUND_SFX_VOICE].duty = 128;
    sndVoice[SOUND_SFX_VOICE].wave = WAVE_PULSE;
    return true;
}

void soundOff()
{
    sndVoice[SOUND_SFX_VOICE].amp = 0;
}

/**
 * Startet ein Musikstück auf den Stimmen 1..3.
 * Ein laufendes Stück wird ersetzt; NULL beendet die Musik.
 */
void music_play(const SoundMusic *stueck)
{
    for (int v = 1; v < SOUND_VOICES; v++)
    {
        sndVoice[v].amp = 0;
        sndSeqIndex[v] = 0;
        sndSeqLeft[v] = 0;
    }
    sndMusic = stueck;
    sndMusicDone = (stueck == NULL);
}

void music_stop() { music_play(NULL); }
bool music_playing() { return !sndMusicDone; }

/**
 * Beibehalten aus der alten Schnittstelle: settings_eeprom_write() ruft das um
 * jeden Flash-Zugriff. Nötig ist es nicht mehr — es gibt keinen Interrupt, der
 * abstürzen könnte — aber während einer Flash-Operation stockt der Task, und
 * stumm klingt das besser als ein Knacken.
 */
void sound_pause() { sndMuted = true; }
void sound_resume() { sndMuted = false; }

/* ------------------------------------------------------------------- Motor */

/** Rückt die Notenlisten um `samples` Abtastwerte vor. */
static void sndAdvanceMusic(uint32_t samples)
{
    if (!sndMusic)
        return;

    bool nochAktiv = false;
    for (int v = 1; v < SOUND_VOICES; v++)
    {
        const SoundTrack &t = sndMusic->voice[v];
        if (!t.notes || t.count == 0)
            continue;

        sndSeqLeft[v] -= (int32_t)samples;
        while (sndSeqLeft[v] <= 0)
        {
            if (sndSeqIndex[v] >= t.count)
            {
                if (!sndMusic->loop)
                {
                    sndVoice[v].amp = 0;
                    sndSeqLeft[v] = 0x7FFFFFFF; // fertig, nicht mehr weiterzählen
                    break;
                }
                sndSeqIndex[v] = 0;
            }
            const SoundNote &n = t.notes[sndSeqIndex[v]++];
            if (n.freq == 0)
            {
                sndVoice[v].amp = 0;
            }
            else
            {
                sndVoice[v].step = sndStepFor(n.freq);
                sndVoice[v].amp = n.amp;
                sndVoice[v].duty = (v == 3) ? 64 : 128; // dritte Stimme schmaler, klingt heller
                sndVoice[v].wave = WAVE_PULSE;
            }
            sndSeqLeft[v] += (int32_t)((uint32_t)n.ms * SOUND_RATE / 1000);
        }
        if (sndSeqIndex[v] < t.count || sndMusic->loop)
            nochAktiv = true;
    }
    sndMusicDone = !nochAktiv;
}

static void sndFillTask(void *)
{
    static uint16_t puffer[SOUND_DMA_LEN * 2]; // je Bild links und rechts

    for (;;)
    {
        sndAdvanceMusic(SOUND_DMA_LEN);

        for (int i = 0; i < SOUND_DMA_LEN; i++)
        {
            int32_t mix = 0;

            for (int v = 0; v < SOUND_VOICES; v++)
            {
                SoundVoice &s = sndVoice[v];
                if (s.amp == 0)
                    continue;
                s.phase += s.step;

                int16_t wert;
                if (s.wave == WAVE_NOISE)
                {
                    // 15-Bit-Schieberegister wie in den Klangchips der Zeit
                    sndLfsr = (sndLfsr >> 1) | (((sndLfsr ^ (sndLfsr >> 1)) & 1) << 14);
                    wert = (sndLfsr & 1) ? s.amp : -(int16_t)s.amp;
                }
                else
                {
                    wert = ((s.phase >> 24) < s.duty) ? s.amp : -(int16_t)s.amp;
                }
                mix += wert >> 1; // Kopffreiheit: eine Stimme allein erreicht Vollausschlag
            }

            if (sndMuted)
                mix = 0;
            if (mix > 127) mix = 127;      // weiches Begrenzen statt Überlauf —
            if (mix < -127) mix = -127;    // klingt wie Übersteuerung, nicht wie Bruch

            // Der DAC nimmt die oberen 8 Bit des 16-Bit-Wortes, Nullpunkt in der Mitte.
            uint16_t s16 = (uint16_t)((128 + mix) << 8);
            puffer[i * 2] = s16;
            puffer[i * 2 + 1] = s16;
        }

        size_t geschrieben = 0;
        // Blockiert, bis die DMA Platz hat — das ist der Takt dieses Tasks.
        i2s_write(I2S_NUM_0, puffer, sizeof(puffer), &geschrieben, portMAX_DELAY);
    }
}

void sound_init()
{
    for (int v = 0; v < SOUND_VOICES; v++)
    {
        sndVoice[v] = {0, 0, 0, 128, WAVE_PULSE};
        sndSeqIndex[v] = 0;
        sndSeqLeft[v] = 0;
    }

    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
    cfg.sample_rate = SOUND_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_MSB;
    cfg.intr_alloc_flags = 0;
    cfg.dma_buf_count = SOUND_DMA_COUNT;
    cfg.dma_buf_len = SOUND_DMA_LEN;
    cfg.use_apll = false;

    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK)
    {
        Serial.println("[sound] I2S konnte nicht gestartet werden - kein Ton");
        return;
    }
    i2s_set_dac_mode(I2S_DAC_CHANNEL_RIGHT_EN); // = GPIO 25
    i2s_zero_dma_buffer(I2S_NUM_0);

    // Kern 0: die Spielschleife läuft auf Kern 1 und ist die belebtere Seite.
    // Höhere Priorität als der FastLED-Task, weil ein Aussetzer im Ton hörbar
    // ist, ein um eine Millisekunde verspätetes Bild dagegen nicht.
    xTaskCreatePinnedToCore(sndFillTask, "snd", 4096, NULL, 3, &sndTaskHandle, 0);

    Serial.printf("[sound] I2S auf den internen DAC (GPIO %d), %d Hz, %d Stimmen\r\n",
                  DAC_AUDIO_PIN, SOUND_RATE, SOUND_VOICES);
}

#endif
