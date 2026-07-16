#include "musicxmlwrite.h"

#include <stdio.h>
#include <stdlib.h>

/* 4/4 with 16th-note ticks -> 16 ticks per measure. divisions = per quarter,
   so a 16th note is exactly 1 division and a note's tick-duration is its
   MusicXML <duration>. */
#define MEASURE_TICKS 16
#define DIVISIONS     4

/* Representable note values, largest first, for greedy duration splitting.
   Every length in 1..16 decomposes because 1 (a 16th) is present. */
static const struct { int ticks; const char *type; int dots; } DUR[] = {
    { 16, "whole",   0 },
    { 12, "half",    1 },
    {  8, "half",    0 },
    {  6, "quarter", 1 },
    {  4, "quarter", 0 },
    {  3, "eighth",  1 },
    {  2, "eighth",  0 },
    {  1, "16th",    0 },
};
#define NDUR ((int)(sizeof(DUR) / sizeof(DUR[0])))

/* Per-channel notation setup. */
static const struct {
    const char *name;
    int         pitched;    /* 0 = unpitched percussion */
    const char *clef_sign;
    int         clef_line;  /* 0 = no line (percussion) */
} PART[NUM_CHANNELS] = {
    [CH_LEAD]     = { "Lead",     1, "G", 2 },
    [CH_HARMONY]  = { "Harmony",  1, "G", 2 },
    [CH_BASS]     = { "Bass",     1, "F", 4 },
    [CH_ARPEGGIO] = { "Arpeggio", 1, "G", 2 },
    [CH_PAD]      = { "Pad",      1, "G", 2 },
    [CH_DRUMS]    = { "Drums",    0, "percussion", 0 },
};

/* ── Monophonic reduction ──────────────────────────────────────────── */

typedef struct { int onset, dur, note; } Mono;

static int mono_cmp(const void *a, const void *b)
{
    const Mono *x = a, *y = b;
    if (x->onset != y->onset)
        return x->onset - y->onset;
    return y->note - x->note;   /* higher pitch first */
}

/* Collect one channel's events, keep the top note per onset, and clip each
   note so it never overlaps the next. Returns the note count. */
static int build_mono(int ch, const MusicEvent *ev, int n, Mono **out)
{
    Mono *m = malloc(sizeof(Mono) * (size_t)(n > 0 ? n : 1));
    if (!m) { *out = NULL; return 0; }

    int c = 0;
    for (int i = 0; i < n; i++) {
        if (ev[i].channel != ch)
            continue;
        if (ev[i].midi_note <= 0 || ev[i].midi_note > 127)
            continue;
        int dur = ev[i].duration_ticks > 0 ? ev[i].duration_ticks : 1;
        m[c].onset = ev[i].tick;
        m[c].dur   = dur;
        m[c].note  = ev[i].midi_note;
        c++;
    }

    qsort(m, (size_t)c, sizeof(Mono), mono_cmp);

    /* drop duplicate onsets (keep first = highest pitch) */
    int w = 0;
    for (int i = 0; i < c; i++) {
        if (w > 0 && m[i].onset == m[w - 1].onset)
            continue;
        m[w++] = m[i];
    }
    c = w;

    /* clip overlaps so the voice stays monophonic */
    for (int i = 0; i + 1 < c; i++) {
        if (m[i].onset + m[i].dur > m[i + 1].onset)
            m[i].dur = m[i + 1].onset - m[i].onset;
        if (m[i].dur < 1)
            m[i].dur = 1;
    }

    *out = m;
    return c;
}

/* ── Note emission ─────────────────────────────────────────────────── */

static int decompose(int len, int *idx)
{
    int k = 0;
    while (len > 0) {
        for (int d = 0; d < NDUR; d++) {
            if (DUR[d].ticks <= len) {
                idx[k++] = d;
                len -= DUR[d].ticks;
                break;
            }
        }
    }
    return k;
}

static void emit_pitch(FILE *fp, int note)
{
    static const char STEP[12] = { 'C','C','D','D','E','F','F','G','G','A','A','B' };
    static const int  ALT [12] = {  0,  1,  0,  1,  0,  0,  1,  0,  1,  0,  1,  0  };
    int pc  = note % 12;
    int oct = note / 12 - 1;   /* MIDI 60 = C4 */
    fprintf(fp, "        <pitch><step>%c</step>", STEP[pc]);
    if (ALT[pc])
        fprintf(fp, "<alter>1</alter>");
    fprintf(fp, "<octave>%d</octave></pitch>\n", oct);
}

static void emit_unpitched(FILE *fp, int note)
{
    char step = 'E';
    int  oct  = 4;
    switch (note) {
        case 36: step = 'F'; oct = 4; break;  /* kick  */
        case 38: step = 'C'; oct = 5; break;  /* snare */
        case 42: step = 'G'; oct = 5; break;  /* hihat */
        case 49: step = 'A'; oct = 5; break;  /* crash */
        default: break;
    }
    fprintf(fp, "        <unpitched><display-step>%c</display-step>"
                "<display-octave>%d</display-octave></unpitched>\n", step, oct);
}

static void emit_one(FILE *fp, int pitched, int note, int d,
                     int tied_prev, int tied_next)
{
    fprintf(fp, "      <note>\n");
    if (pitched)
        emit_pitch(fp, note);
    else
        emit_unpitched(fp, note);
    fprintf(fp, "        <duration>%d</duration>\n", DUR[d].ticks);
    if (tied_prev) fprintf(fp, "        <tie type=\"stop\"/>\n");
    if (tied_next) fprintf(fp, "        <tie type=\"start\"/>\n");
    fprintf(fp, "        <voice>1</voice>\n        <type>%s</type>\n", DUR[d].type);
    if (DUR[d].dots)
        fprintf(fp, "        <dot/>\n");
    if (tied_prev || tied_next) {
        fprintf(fp, "        <notations>\n");
        if (tied_prev) fprintf(fp, "          <tied type=\"stop\"/>\n");
        if (tied_next) fprintf(fp, "          <tied type=\"start\"/>\n");
        fprintf(fp, "        </notations>\n");
    }
    fprintf(fp, "      </note>\n");
}

/* A sounding note segment within one measure. `cont` = continues a note tied
   in from a previous measure; `more` = extends past this segment. */
static void emit_note_seg(FILE *fp, int pitched, int note, int len,
                          int cont, int more)
{
    int idx[64];
    int k = decompose(len, idx);
    for (int j = 0; j < k; j++)
        emit_one(fp, pitched, note, idx[j],
                 cont || j > 0, more || j < k - 1);
}

static void emit_rest_seg(FILE *fp, int len)
{
    int idx[64];
    int k = decompose(len, idx);
    for (int j = 0; j < k; j++) {
        fprintf(fp, "      <note>\n        <rest/>\n"
                    "        <duration>%d</duration>\n"
                    "        <voice>1</voice>\n        <type>%s</type>\n",
                DUR[idx[j]].ticks, DUR[idx[j]].type);
        if (DUR[idx[j]].dots)
            fprintf(fp, "        <dot/>\n");
        fprintf(fp, "      </note>\n");
    }
}

/* ── Part emission ─────────────────────────────────────────────────── */

static void emit_part(FILE *fp, int ch, const Mono *m, int count,
                      int measures, float bpm, int with_tempo)
{
    int pos = 0, ni = 0;

    for (int meas = 0; meas < measures; meas++) {
        int me = (meas + 1) * MEASURE_TICKS;
        fprintf(fp, "    <measure number=\"%d\">\n", meas + 1);

        if (meas == 0) {
            fprintf(fp, "      <attributes>\n"
                        "        <divisions>%d</divisions>\n"
                        "        <key><fifths>0</fifths></key>\n"
                        "        <time><beats>4</beats><beat-type>4</beat-type></time>\n",
                    DIVISIONS);
            if (PART[ch].clef_line > 0)
                fprintf(fp, "        <clef><sign>%s</sign><line>%d</line></clef>\n",
                        PART[ch].clef_sign, PART[ch].clef_line);
            else
                fprintf(fp, "        <clef><sign>%s</sign></clef>\n",
                        PART[ch].clef_sign);
            fprintf(fp, "      </attributes>\n");

            if (with_tempo) {
                int tempo = (int)(bpm > 1.0f ? bpm : 120.0f);
                fprintf(fp, "      <direction placement=\"above\">\n"
                            "        <direction-type>\n"
                            "          <metronome><beat-unit>quarter</beat-unit>"
                            "<per-minute>%d</per-minute></metronome>\n"
                            "        </direction-type>\n"
                            "        <sound tempo=\"%d\"/>\n"
                            "      </direction>\n", tempo, tempo);
            }
        }

        while (pos < me) {
            if (ni < count && m[ni].onset <= pos) {
                int note_end = m[ni].onset + m[ni].dur;
                if (pos < note_end) {
                    int seg_end = note_end < me ? note_end : me;
                    emit_note_seg(fp, PART[ch].pitched, m[ni].note,
                                  seg_end - pos,
                                  pos > m[ni].onset, seg_end < note_end);
                    pos = seg_end;
                    if (pos >= note_end)
                        ni++;
                    continue;
                }
                ni++;   /* note already ended (shouldn't happen) */
                continue;
            }
            /* gap -> rest up to the next onset or the barline */
            int next = (ni < count && m[ni].onset < me) ? m[ni].onset : me;
            emit_rest_seg(fp, next - pos);
            pos = next;
        }

        fprintf(fp, "    </measure>\n");
    }
}

/* ── File writer ───────────────────────────────────────────────────── */

int musicxml_write(const char *path, const MusicEvent *events, int num_events,
                   float bpm)
{
    Mono *mono[NUM_CHANNELS];
    int   counts[NUM_CHANNELS];
    int   maxend = 0;

    for (int c = 0; c < NUM_CHANNELS; c++) {
        counts[c] = build_mono(c, events, num_events, &mono[c]);
        if (counts[c] > 0) {
            int end = mono[c][counts[c] - 1].onset + mono[c][counts[c] - 1].dur;
            if (end > maxend)
                maxend = end;
        }
    }

    int measures = (maxend + MEASURE_TICKS - 1) / MEASURE_TICKS;
    if (measures < 1)
        measures = 1;

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "error: cannot open '%s' for writing\n", path);
        for (int c = 0; c < NUM_CHANNELS; c++) free(mono[c]);
        return -1;
    }

    fprintf(fp,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE score-partwise PUBLIC \"-//Recordare//DTD MusicXML 3.1 Partwise//EN\" "
        "\"http://www.musicxml.org/dtds/partwise.dtd\">\n"
        "<score-partwise version=\"3.1\">\n"
        "  <work><work-title>cheapbin</work-title></work>\n"
        "  <part-list>\n");
    for (int c = 0; c < NUM_CHANNELS; c++)
        fprintf(fp, "    <score-part id=\"P%d\"><part-name>%s</part-name></score-part>\n",
                c + 1, PART[c].name);
    fprintf(fp, "  </part-list>\n");

    for (int c = 0; c < NUM_CHANNELS; c++) {
        fprintf(fp, "  <part id=\"P%d\">\n", c + 1);
        emit_part(fp, c, mono[c], counts[c], measures, bpm, c == 0);
        fprintf(fp, "  </part>\n");
    }

    fprintf(fp, "</score-partwise>\n");

    int rc = (fclose(fp) == 0) ? 0 : -1;
    for (int c = 0; c < NUM_CHANNELS; c++)
        free(mono[c]);
    return rc;
}
