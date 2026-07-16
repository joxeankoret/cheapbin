#include "wavwrite.h"
#include <stdlib.h>
#include <string.h>

/* Little-endian scalar writers. */
static int put_u32(FILE *fp, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8),
                     (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return fwrite(b, 1, 4, fp) == 4 ? 0 : -1;
}

static int put_u16(FILE *fp, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    return fwrite(b, 1, 2, fp) == 2 ? 0 : -1;
}

static int put_tag(FILE *fp, const char *tag)
{
    return fwrite(tag, 1, 4, fp) == 4 ? 0 : -1;
}

WavWriter *wav_open(const char *path, int sample_rate, int channels)
{
    WavWriter *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;

    w->fp = fopen(path, "wb");
    if (!w->fp) {
        fprintf(stderr, "error: cannot open '%s' for writing\n", path);
        free(w);
        return NULL;
    }

    const uint16_t bits        = 16;
    const uint16_t block_align = (uint16_t)(channels * (bits / 8));
    const uint32_t byte_rate   = (uint32_t)sample_rate * block_align;

    /* Header. data/RIFF sizes are placeholders, patched in wav_close(). */
    int ok = 0;
    ok |= put_tag(w->fp, "RIFF");
    ok |= put_u32(w->fp, 0);                 /* riff size  (patched) */
    ok |= put_tag(w->fp, "WAVE");
    ok |= put_tag(w->fp, "fmt ");
    ok |= put_u32(w->fp, 16);                /* fmt chunk size */
    ok |= put_u16(w->fp, 1);                 /* PCM */
    ok |= put_u16(w->fp, (uint16_t)channels);
    ok |= put_u32(w->fp, (uint32_t)sample_rate);
    ok |= put_u32(w->fp, byte_rate);
    ok |= put_u16(w->fp, block_align);
    ok |= put_u16(w->fp, bits);
    ok |= put_tag(w->fp, "data");
    ok |= put_u32(w->fp, 0);                 /* data size (patched) */

    if (ok != 0) {
        fprintf(stderr, "error: failed to write WAV header for '%s'\n", path);
        fclose(w->fp);
        free(w);
        return NULL;
    }

    return w;
}

int wav_write(WavWriter *w, const int16_t *samples, int num_samples)
{
    if (num_samples <= 0)
        return 0;

    /* The engine produces host-endian int16; on the supported little-endian
       targets this maps straight to the file. Write byte-by-byte to stay
       correct regardless of host endianness. */
    for (int i = 0; i < num_samples; i++) {
        if (put_u16(w->fp, (uint16_t)samples[i]) != 0)
            return -1;
    }
    w->data_bytes += (uint32_t)num_samples * sizeof(int16_t);
    return 0;
}

int wav_close(WavWriter *w)
{
    if (!w)
        return -1;

    int rc = 0;

    /* Patch data chunk size (offset 40) and RIFF size (offset 4). */
    if (fseek(w->fp, 40, SEEK_SET) != 0 || put_u32(w->fp, w->data_bytes) != 0)
        rc = -1;
    if (fseek(w->fp, 4, SEEK_SET) != 0 ||
        put_u32(w->fp, 36 + w->data_bytes) != 0)
        rc = -1;

    if (fclose(w->fp) != 0)
        rc = -1;
    free(w);
    return rc;
}
