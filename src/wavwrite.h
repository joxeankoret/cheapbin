#ifndef CHEAPBIN_WAVWRITE_H
#define CHEAPBIN_WAVWRITE_H

#include <stdint.h>
#include <stdio.h>

/* Minimal streaming writer for canonical PCM WAV files.
   Matches the synth engine's output: 16-bit signed, little-endian.
   Sizes in the RIFF header are patched in on wav_close(). */

typedef struct {
    FILE   *fp;
    uint32_t data_bytes;   /* running total of PCM payload written */
} WavWriter;

/* Open a WAV file for writing. Returns NULL on error (and prints why). */
WavWriter *wav_open(const char *path, int sample_rate, int channels);

/* Append num_samples signed 16-bit samples (interleaved if channels > 1). */
int wav_write(WavWriter *w, const int16_t *samples, int num_samples);

/* Patch the header sizes and close. Frees the writer. */
int wav_close(WavWriter *w);

#endif
