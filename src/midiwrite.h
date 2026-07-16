#ifndef CHEAPBIN_MIDIWRITE_H
#define CHEAPBIN_MIDIWRITE_H

#include "composer.h"

/* Write the composition's note events as a Standard MIDI File (format 1):
   a tempo/meta track followed by one track per cheapbin channel, so a DAW
   imports each channel (lead, harmony, bass, arpeggio, pad, drums) as its
   own editable track. Drums land on MIDI channel 10 (GM percussion); their
   note numbers are already GM drum notes.

   Pass the *transformed* event list (i.e. after any style is applied) so the
   MIDI matches what the audio output produces. Returns 0 on success. */
int midi_write(const char *path, const MusicEvent *events, int num_events,
               float bpm);

#endif
