#ifndef CHEAPBIN_MUSICXMLWRITE_H
#define CHEAPBIN_MUSICXMLWRITE_H

#include "composer.h"

/* Write the composition's note events as a MusicXML 3.1 score-partwise file:
   one part per cheapbin channel (lead, harmony, bass, arpeggio, pad, drums),
   in 4/4 with the song's tempo, ready to open in MuseScore or any notation
   editor.

   MusicXML is a notation format, so each part is reduced to a single
   monophonic voice (the top note at each onset; overlapping notes are
   clipped). Chords collapse to their top note — use the MIDI export for full
   polyphony. Pass the *transformed* event list (after any style) so the score
   matches the audio. Returns 0 on success. */
int musicxml_write(const char *path, const MusicEvent *events, int num_events,
                   float bpm);

#endif
