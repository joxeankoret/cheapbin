#ifndef CHEAPBIN_OUTPUT_H
#define CHEAPBIN_OUTPUT_H

#include "composer.h"
#include "chipemu.h"
#include "style.h"
#include <stdbool.h>

/* Offline WAV output. Only ever invoked when the user passes an output flag;
   it never touches the audio device or the display. */

typedef struct {
    const char *mixed_path;     /* --output <file>: single mixed WAV, or NULL */
    const char *tracks_prefix;  /* --output-tracks <prefix>: one WAV per
                                   channel (<prefix>_lead.wav, ...), or NULL */
    const char *midi_path;      /* --output-midi <file>: multi-track .mid,
                                   or NULL */
    const char *musicxml_path;  /* --output-musicxml <file>: score, or NULL */
    bool        apply_fx;       /* false if --no-fx: emit the raw channel mix
                                   without cheapbin's effect chain (WAV only) */
} OutputOptions;

/* Render the composition to disk per opts. Uses the same chip/style the
   interactive player would. Returns 0 on success, -1 on any failure. */
int output_run(Composition *comp, ChipType chip, StyleType style,
               const OutputOptions *opts);

#endif
