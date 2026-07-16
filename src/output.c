#include "output.h"
#include "synth.h"
#include "wavwrite.h"
#include "midiwrite.h"
#include "musicxmlwrite.h"

#include <stdio.h>
#include <stdlib.h>

#define OUT_CHUNK 4096   /* samples rendered per synth_render call */

/* Per-channel track file suffixes, indexed by channel id (composer.h). */
static const char *const CHANNEL_SUFFIX[NUM_CHANNELS] = {
    "lead", "harmony", "bass", "arpeggio", "pad", "drums",
};

/* Render one synth configuration (channel mask + fx setting) to a WAV file.
   Rebuilds the synth from scratch so every pass starts at tick 0 with
   identical, deterministic state. */
static int render_pass(Composition *comp, ChipType chip, StyleType style,
                       uint32_t channel_mask, bool apply_fx,
                       const char *path)
{
    SynthState synth;
    synth_init(&synth, comp);
    synth_set_chip(&synth, chip);
    synth_apply_style(&synth, style, comp);
    synth.channel_mask = channel_mask;
    synth.apply_fx     = apply_fx;

    WavWriter *w = wav_open(path, SAMPLE_RATE, 1);
    if (!w) {
        free(synth.styled_events);
        return -1;
    }

    fprintf(stderr, "  writing %s ...", path);
    fflush(stderr);

    int16_t buf[OUT_CHUNK];
    int rc = 0;
    /* Check finished before each call: synth_render zero-fills once the song
       is over, so stopping here means we never append a trailing silent
       buffer. The buffer that trips `finished` still carries its real tail. */
    while (!synth.finished) {
        synth_render(&synth, buf, OUT_CHUNK);
        if (wav_write(w, buf, OUT_CHUNK) != 0) {
            fprintf(stderr, " write failed\n");
            rc = -1;
            break;
        }
    }

    if (wav_close(w) != 0)
        rc = -1;

    if (rc == 0)
        fprintf(stderr, " done\n");

    free(synth.styled_events);
    return rc;
}

/* Write the note events (after style transformation) to a MIDI file. */
static int write_midi(Composition *comp, StyleType style, const char *path)
{
    SynthState synth;
    synth_init(&synth, comp);
    synth_apply_style(&synth, style, comp);  /* sets events/num_events/bpm */

    fprintf(stderr, "  writing %s ...", path);
    fflush(stderr);

    int rc = midi_write(path, synth.events, synth.num_events, synth.bpm);
    fprintf(stderr, rc == 0 ? " done\n" : " failed\n");

    free(synth.styled_events);
    return rc;
}

/* Write the note events (after style transformation) to a MusicXML file. */
static int write_musicxml(Composition *comp, StyleType style, const char *path)
{
    SynthState synth;
    synth_init(&synth, comp);
    synth_apply_style(&synth, style, comp);  /* sets events/num_events/bpm */

    fprintf(stderr, "  writing %s ...", path);
    fflush(stderr);

    int rc = musicxml_write(path, synth.events, synth.num_events, synth.bpm);
    fprintf(stderr, rc == 0 ? " done\n" : " failed\n");

    free(synth.styled_events);
    return rc;
}

int output_run(Composition *comp, ChipType chip, StyleType style,
               const OutputOptions *opts)
{
    int rc = 0;

    if (opts->midi_path) {
        fprintf(stderr, "cheapbin: writing MIDI output\n");
        if (write_midi(comp, style, opts->midi_path) != 0)
            rc = -1;
    }

    if (opts->musicxml_path) {
        fprintf(stderr, "cheapbin: writing MusicXML output\n");
        if (write_musicxml(comp, style, opts->musicxml_path) != 0)
            rc = -1;
    }

    if (!opts->mixed_path && !opts->tracks_prefix)
        return rc;

    fprintf(stderr, "cheapbin: writing WAV output (%s)\n",
            opts->apply_fx ? "with effects" : "raw, no effects");

    if (opts->mixed_path) {
        if (render_pass(comp, chip, style, 0xFFFFFFFFu, opts->apply_fx,
                        opts->mixed_path) != 0)
            rc = -1;
    }

    if (opts->tracks_prefix) {
        for (int c = 0; c < NUM_CHANNELS; c++) {
            char path[1024];
            int n = snprintf(path, sizeof(path), "%s_%s.wav",
                             opts->tracks_prefix, CHANNEL_SUFFIX[c]);
            if (n < 0 || n >= (int)sizeof(path)) {
                fprintf(stderr, "error: output track path too long\n");
                rc = -1;
                continue;
            }
            if (render_pass(comp, chip, style, (1u << c), opts->apply_fx,
                            path) != 0)
                rc = -1;
        }
    }

    return rc;
}
