#include "midiwrite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MIDI resolution. 480 ticks per quarter note is a common DAW default.
   cheapbin ticks are 16th notes, so one cheapbin tick = PPQ/4 MIDI ticks. */
#define PPQ           480
#define TICKS_PER_16  (PPQ / 4)

/* GM programs approximating each channel's role (cosmetic; reassign in DAW).
   Drums use MIDI channel 10 and take no program change. */
static const struct {
    const char *name;
    int         program;   /* 0-based GM program, -1 = none (drums) */
} CH_INFO[NUM_CHANNELS] = {
    [CH_LEAD]     = { "Lead",     80 },  /* Lead 1 (square)   */
    [CH_HARMONY]  = { "Harmony",  81 },  /* Lead 2 (sawtooth) */
    [CH_BASS]     = { "Bass",     38 },  /* Synth Bass 1      */
    [CH_ARPEGGIO] = { "Arpeggio", 82 },  /* Lead 3 (calliope) */
    [CH_PAD]      = { "Pad",      88 },  /* Pad 1 (new age)   */
    [CH_DRUMS]    = { "Drums",    -1 },  /* GM percussion     */
};

/* ── Growable byte buffer ──────────────────────────────────────────── */

typedef struct { uint8_t *data; size_t len, cap; } ByteBuf;

static int bb_reserve(ByteBuf *b, size_t extra)
{
    if (b->len + extra <= b->cap)
        return 0;
    size_t cap = b->cap ? b->cap * 2 : 256;
    while (cap < b->len + extra)
        cap *= 2;
    uint8_t *p = realloc(b->data, cap);
    if (!p)
        return -1;
    b->data = p;
    b->cap  = cap;
    return 0;
}

static void bb_byte(ByteBuf *b, uint8_t v)
{
    if (bb_reserve(b, 1) == 0)
        b->data[b->len++] = v;
}

static void bb_bytes(ByteBuf *b, const void *p, size_t n)
{
    if (bb_reserve(b, n) == 0) {
        memcpy(b->data + b->len, p, n);
        b->len += n;
    }
}

/* MIDI variable-length quantity (7 bits/byte, high bit = continues). */
static void bb_vlq(ByteBuf *b, uint32_t v)
{
    uint8_t tmp[5];
    int n = 0;
    tmp[n++] = v & 0x7F;
    while ((v >>= 7))
        tmp[n++] = (v & 0x7F) | 0x80;
    while (n--)
        bb_byte(b, tmp[n]);
}

/* ── Note-event flattening ─────────────────────────────────────────── */

typedef struct {
    int      tick;   /* absolute cheapbin tick */
    uint8_t  on;     /* 1 = note on, 0 = note off */
    uint8_t  note;
    uint8_t  vel;
} NoteEv;

static int noteev_cmp(const void *pa, const void *pb)
{
    const NoteEv *a = pa, *b = pb;
    if (a->tick != b->tick)
        return a->tick - b->tick;
    /* at equal tick, emit note-offs before note-ons */
    return (int)a->on - (int)b->on;
}

/* ── Track assembly ────────────────────────────────────────────────── */

static void put_track_name(ByteBuf *t, const char *name)
{
    size_t len = strlen(name);
    bb_vlq(t, 0);                       /* delta 0 */
    bb_byte(t, 0xFF); bb_byte(t, 0x03); /* meta: track name */
    bb_vlq(t, (uint32_t)len);
    bb_bytes(t, name, len);
}

static void put_end_of_track(ByteBuf *t)
{
    bb_vlq(t, 0);
    bb_byte(t, 0xFF); bb_byte(t, 0x2F); bb_byte(t, 0x00);
}

/* Build the tempo/meta conductor track. */
static void build_tempo_track(ByteBuf *t, float bpm)
{
    put_track_name(t, "cheapbin");

    /* time signature 4/4 */
    bb_vlq(t, 0);
    bb_byte(t, 0xFF); bb_byte(t, 0x58); bb_byte(t, 0x04);
    bb_byte(t, 0x04); bb_byte(t, 0x02); bb_byte(t, 0x18); bb_byte(t, 0x08);

    /* tempo: microseconds per quarter note */
    uint32_t upq = (uint32_t)(60000000.0f / (bpm > 1.0f ? bpm : 120.0f));
    bb_vlq(t, 0);
    bb_byte(t, 0xFF); bb_byte(t, 0x51); bb_byte(t, 0x03);
    bb_byte(t, (uint8_t)(upq >> 16));
    bb_byte(t, (uint8_t)(upq >> 8));
    bb_byte(t, (uint8_t)(upq));

    put_end_of_track(t);
}

/* Build one channel track. Returns 1 if it holds any notes, else 0. */
static int build_channel_track(ByteBuf *t, int ch,
                               const MusicEvent *events, int num_events)
{
    int midi_ch = (ch == CH_DRUMS) ? 9 : ch;

    /* flatten this channel's events into on/off points */
    NoteEv *nev = NULL;
    int count = 0, cap = 0;
    for (int i = 0; i < num_events; i++) {
        const MusicEvent *e = &events[i];
        if (e->channel != ch)
            continue;
        if (e->midi_note <= 0 || e->midi_note > 127)
            continue;   /* rest / out of range */

        int dur = e->duration_ticks > 0 ? e->duration_ticks : 1;
        int vel = e->velocity;
        if (vel < 1)   vel = 1;
        if (vel > 127) vel = 127;

        if (count + 2 > cap) {
            cap = cap ? cap * 2 : 64;
            NoteEv *p = realloc(nev, (size_t)cap * sizeof(NoteEv));
            if (!p) { free(nev); return 0; }
            nev = p;
        }
        nev[count++] = (NoteEv){ e->tick,           1,
                                 (uint8_t)e->midi_note, (uint8_t)vel };
        nev[count++] = (NoteEv){ e->tick + dur,      0,
                                 (uint8_t)e->midi_note, 0 };
    }

    if (count == 0) {
        free(nev);
        return 0;
    }

    qsort(nev, (size_t)count, sizeof(NoteEv), noteev_cmp);

    put_track_name(t, CH_INFO[ch].name);

    if (CH_INFO[ch].program >= 0) {
        bb_vlq(t, 0);
        bb_byte(t, (uint8_t)(0xC0 | midi_ch));
        bb_byte(t, (uint8_t)CH_INFO[ch].program);
    }

    int last_midi_tick = 0;
    for (int i = 0; i < count; i++) {
        int midi_tick = nev[i].tick * TICKS_PER_16;
        uint32_t delta = (uint32_t)(midi_tick - last_midi_tick);
        last_midi_tick = midi_tick;

        bb_vlq(t, delta);
        if (nev[i].on) {
            bb_byte(t, (uint8_t)(0x90 | midi_ch));
            bb_byte(t, nev[i].note);
            bb_byte(t, nev[i].vel);
        } else {
            bb_byte(t, (uint8_t)(0x80 | midi_ch));
            bb_byte(t, nev[i].note);
            bb_byte(t, 0);
        }
    }

    put_end_of_track(t);
    free(nev);
    return 1;
}

/* ── File writer ───────────────────────────────────────────────────── */

static int write_chunk(FILE *fp, const char *tag, const ByteBuf *body)
{
    uint32_t n = (uint32_t)body->len;
    uint8_t len[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16),
                       (uint8_t)(n >> 8),  (uint8_t)n };
    if (fwrite(tag, 1, 4, fp) != 4) return -1;
    if (fwrite(len, 1, 4, fp) != 4) return -1;
    if (body->len && fwrite(body->data, 1, body->len, fp) != body->len)
        return -1;
    return 0;
}

int midi_write(const char *path, const MusicEvent *events, int num_events,
               float bpm)
{
    /* Build every track in memory first: we need the final track count for
       the header, and each chunk needs its byte length up front. */
    ByteBuf tracks[1 + NUM_CHANNELS];
    memset(tracks, 0, sizeof(tracks));
    int ntracks = 0;

    build_tempo_track(&tracks[ntracks++], bpm);

    for (int c = 0; c < NUM_CHANNELS; c++) {
        ByteBuf *t = &tracks[ntracks];
        if (build_channel_track(t, c, events, num_events))
            ntracks++;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "error: cannot open '%s' for writing\n", path);
        for (int i = 0; i < ntracks; i++) free(tracks[i].data);
        return -1;
    }

    /* MThd */
    uint8_t hdr[14] = {
        'M','T','h','d', 0,0,0,6,
        0,1,                                   /* format 1              */
        (uint8_t)(ntracks >> 8), (uint8_t)ntracks,
        (uint8_t)(PPQ >> 8), (uint8_t)PPQ,     /* division (ticks/quarter) */
    };
    int rc = 0;
    if (fwrite(hdr, 1, sizeof(hdr), fp) != sizeof(hdr))
        rc = -1;

    for (int i = 0; i < ntracks && rc == 0; i++) {
        if (write_chunk(fp, "MTrk", &tracks[i]) != 0)
            rc = -1;
    }

    if (fclose(fp) != 0)
        rc = -1;
    for (int i = 0; i < ntracks; i++)
        free(tracks[i].data);

    return rc;
}
