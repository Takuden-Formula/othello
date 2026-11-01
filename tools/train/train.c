/*
 * Trains the pattern evaluation from self-play data and writes src/weights.c.
 *   train <out weights.c> <iterations> data1.bin [data2.bin ...]
 * Data: records of 17 bytes (P, O as uint64, final disc difference from P as int8), see selfplay.c.
 *
 * Model per phase (number of empties, see patterns.h):
 *   value = sum of pattern weights + bias + a*mobility + b*potential mobility + c*parity   [discs]
 * Fitted by least squares, one phase at a time from the end of the game backwards.
 * Targets: the final result is exact once both sides play perfectly (last 14 empties), but early in a
 * game it is mostly noise, so for earlier phases the target is mixed with the already trained
 * evaluation of the same game BOOT_PLIES moves later (temporal-difference style bootstrapping).
 * Weights are stored as 1/16 disc, zero-run + zigzag-varint compressed, in a C string.
 */
#include "../../src/patterns.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define Q 16.0                         /* stored units per disc */
#define BOOT_PLIES (61 / PAT_PHASES + 2) /* look this many discs ahead (always a later, already trained phase) */
#define BOOT_MIX   0.75                /* share of the bootstrapped value in the target */
#define BOOT_EMPTIES 18                /* positions with at least this many empties use bootstrapped targets */

typedef struct { uint64_t p, o; int8_t s; } Rec;

static PatSet PS;
static Rec *recs;
static size_t nrec, rcap;

static int pc(uint64_t x) { return __builtin_popcountll(x); }

#define L1(x) ((x) << 1)
#define R1(x) ((x) >> 1)
#define L7(x) ((x) << 7)
#define R7(x) ((x) >> 7)
#define L8(x) ((x) << 8)
#define R8(x) ((x) >> 8)
#define L9(x) ((x) << 9)
#define R9(x) ((x) >> 9)

static uint64_t moves(uint64_t P, uint64_t O)
{
    const uint64_t IH = 0x7E7E7E7E7E7E7E7EULL;
    uint64_t mO = O & IH, t, m = 0;
#define D(SH, M) t = (M) & SH(P); t |= (M) & SH(t); t |= (M) & SH(t); t |= (M) & SH(t); \
                 t |= (M) & SH(t); t |= (M) & SH(t); m |= SH(t);
    D(L1, mO) D(R1, mO) D(L8, O) D(R8, O) D(L7, mO) D(R7, mO) D(L9, mO) D(R9, mO)
#undef D
    return m & ~(P | O);
}

static uint64_t neighbours(uint64_t x)
{
    uint64_t h = ((x << 1) & 0xFEFEFEFEFEFEFEFEULL) | ((x >> 1) & 0x7F7F7F7F7F7F7F7FULL);
    uint64_t r = x | h;
    return (r << 8) | (r >> 8) | h;
}

/* must match pat_scalars() in engine.c */
static void scalars(uint64_t P, uint64_t O, double *f)
{
    uint64_t E = ~(P | O);
    int ne = pc(E);
    f[0] = 1;
    f[1] = pc(moves(P, O)) - pc(moves(O, P));
    f[2] = pc(neighbours(O) & E) - pc(neighbours(P) & E);
    f[3] = (ne & 1) ? 1 : -1;
}

static void load(const char *fn)
{
    FILE *f = fopen(fn, "rb");
    unsigned char buf[17];
    if (!f) { fprintf(stderr, "cannot open %s\n", fn); exit(1); }
    while (fread(buf, 17, 1, f) == 1) {
        Rec r;
        if (nrec == rcap) { rcap = rcap ? rcap * 2 : 1 << 20; recs = realloc(recs, sizeof(Rec) * rcap); }
        memcpy(&r.p, buf, 8); memcpy(&r.o, buf + 8, 8); r.s = (int8_t)buf[16];
        recs[nrec++] = r;
    }
    fclose(f);
}

/* ---- output: zero runs + zigzag varints */
static unsigned char *ob;
static size_t on, ocap;
static void put(unsigned v)
{
    if (on == ocap) { ocap = ocap ? ocap * 2 : 1 << 20; ob = realloc(ob, ocap); }
    ob[on++] = (unsigned char)v;
}
static void varint(unsigned v) { while (v >= 128) { put((v & 127) | 128); v >>= 7; } put(v); }
static void encode(const int *w, int n)
{
    int i = 0;
    while (i < n) {
        if (w[i] == 0) {
            int j = i;
            while (j < n && w[j] == 0) j++;
            put(0); varint((unsigned)(j - i)); i = j;
        } else {
            int v = w[i++];
            varint(v > 0 ? (unsigned)v * 2 - 1 : (unsigned)(-v) * 2);
        }
    }
}

static double *W[PAT_PHASES], SW[PAT_PHASES][PAT_SCALARS];
static int32_t *nxt;                   /* record BOOT_PLIES discs later in the same game, or -1 */

static double predict(const Rec *r)
{
    uint32_t idx[64];
    double f[PAT_SCALARS], v = 0;
    int ph = pat_phase(64 - pc(r->p | r->o)), i;
    pat_indices(&PS, r->p, r->o, idx);
    scalars(r->p, r->o, f);
    for (i = 0; i < PS.n; i++) v += W[ph][idx[i]];
    for (i = 0; i < PAT_SCALARS; i++) v += SW[ph][i] * f[i];
    return v;
}

static void link_games(void)
{
    size_t k, j;
    nxt = malloc(sizeof(int32_t) * nrec);
    for (k = 0; k < nrec; k++) {
        int d0 = pc(recs[k].p | recs[k].o);
        nxt[k] = -1;
        for (j = k + 1; j < nrec && j < k + 64; j++) {
            int d = pc(recs[j].p | recs[j].o);
            if (d <= pc(recs[j - 1].p | recs[j - 1].o)) break;      /* next game started */
            if (d >= d0 + BOOT_PLIES) { nxt[k] = (int32_t)j; break; }
        }
    }
}

/* Training target of record k in discs, from its side to move. */
static double target(size_t k)
{
    const Rec *a = &recs[k], *b;
    int same;
    double v;
    if (64 - pc(a->p | a->o) < BOOT_EMPTIES || nxt[k] < 0) return a->s;
    b = &recs[nxt[k]];
    if (pat_phase(64 - pc(b->p | b->o)) <= pat_phase(64 - pc(a->p | a->o))) return a->s;
    /* same side to move? the final result tells (it flips sign with the side); for draws use the
     * number of moves in between (passes are rare) */
    same = a->s ? (b->s == a->s) : !((pc(b->p | b->o) - pc(a->p | a->o)) & 1);
    v = predict(b);
    if (!same) v = -v;
    return BOOT_MIX * v + (1 - BOOT_MIX) * a->s;
}

int main(int argc, char **argv)
{
    int iters, ph, i, a, entries;
    int *qall;
    FILE *out;
    if (argc < 4) { fprintf(stderr, "usage: train out.c iterations data.bin...\n"); return 1; }
    iters = atoi(argv[2]);
    pat_build(&PS);
    entries = PS.offset[PAT_TYPES];
    for (a = 3; a < argc; a++) load(argv[a]);
    fprintf(stderr, "%zu positions, %d instances, %d entries/phase\n", nrec, PS.n, entries);
    qall = calloc((size_t)PAT_PHASES * (entries + PAT_SCALARS), sizeof(int));
    link_games();

    for (ph = PAT_PHASES - 1; ph >= 0; ph--) {
        size_t n = 0, k, nval = 0;
        uint32_t *ix;
        float *sf, *tg;
        unsigned char *val;
        double *r;
        double *w = calloc(entries, sizeof(double)), *g = calloc(entries, sizeof(double));
        double *cnt = calloc(entries, sizeof(double));
        double sw[PAT_SCALARS] = { 0 }, mse = 0, vmse = 0;
        int it, worse;
        double best;
        for (k = 0; k < nrec; k++) if (pat_phase(64 - pc(recs[k].p | recs[k].o)) == ph) n++;
        ix = malloc(sizeof(uint32_t) * n * PS.n);
        sf = malloc(sizeof(float) * n * PAT_SCALARS);
        tg = malloc(sizeof(float) * n);
        val = malloc(n);
        r = malloc(sizeof(double) * n);
        n = 0;
        for (k = 0; k < nrec; k++) {
            const Rec *r = &recs[k];
            double f[PAT_SCALARS];
            uint64_t h;
            if (pat_phase(64 - pc(r->p | r->o)) != ph) continue;
            pat_indices(&PS, r->p, r->o, ix + n * PS.n);
            scalars(r->p, r->o, f);
            for (i = 0; i < PAT_SCALARS; i++) sf[n * PAT_SCALARS + i] = (float)f[i];
            tg[n] = (float)target(k);
            h = (r->p * 0x9E3779B97F4A7C15ULL) ^ (r->o * 0xC2B2AE3D27D4EB4FULL);
            h ^= h >> 29;
            val[n] = (h & 31) == 0;           /* ~3% held out for validation */
            nval += val[n];
            n++;
        }
        for (k = 0; k < n; k++) if (!val[k])
            for (i = 0; i < PS.n; i++) cnt[ix[k * PS.n + i]] += 1;

        /* Block coordinate descent: r[] holds the residual (target - prediction) of every position.
         * Each pass updates one pattern shape (its weights move by the mean residual of the positions
         * that use them, divided by the number of instances of that shape), then the 4 scalars by an
         * exact 4x4 least-squares step. Stable, and converges in a few dozen iterations. */
        for (k = 0; k < n; k++) r[k] = tg[k];
        for (it = 0, worse = 0, best = 1e30; it < iters && worse < 3; it++) {
            double se = 0, vse = 0;
            int t;
            for (t = 0; t < PAT_TYPES; t++) {
                int lo = PS.offset[t], hi = PS.offset[t + 1], ninst = 0, j;
                int inst[PAT_MAXLEN];
                for (j = 0; j < PS.n; j++) if (PS.type[j] == t) inst[ninst++] = j;
                memset(g + lo, 0, sizeof(double) * (hi - lo));
                for (k = 0; k < n; k++) if (!val[k])
                    for (j = 0; j < ninst; j++) g[ix[k * PS.n + inst[j]]] += r[k];
                for (i = lo; i < hi; i++) {
                    g[i] = cnt[i] > 0 ? g[i] / (cnt[i] + 10.0) / ninst : 0;   /* the change of this weight */
                    w[i] += g[i];
                }
                for (k = 0; k < n; k++)
                    for (j = 0; j < ninst; j++) r[k] -= g[ix[k * PS.n + inst[j]]];
            }
            {   /* scalars: solve (F'F) d = F'r */
                double A[PAT_SCALARS][PAT_SCALARS + 1] = { { 0 } }, d[PAT_SCALARS];
                int c, rr;
                for (k = 0; k < n; k++) if (!val[k]) {
                    const float *f = sf + k * PAT_SCALARS;
                    for (rr = 0; rr < PAT_SCALARS; rr++) {
                        for (c = 0; c < PAT_SCALARS; c++) A[rr][c] += f[rr] * f[c];
                        A[rr][PAT_SCALARS] += f[rr] * r[k];
                    }
                }
                for (rr = 0; rr < PAT_SCALARS; rr++) A[rr][rr] += 1e-3 * (double)n;
                for (c = 0; c < PAT_SCALARS; c++)
                    for (rr = c + 1; rr < PAT_SCALARS; rr++) {
                        double m = A[rr][c] / A[c][c];
                        int q;
                        for (q = c; q <= PAT_SCALARS; q++) A[rr][q] -= m * A[c][q];
                    }
                for (rr = PAT_SCALARS - 1; rr >= 0; rr--) {
                    double s = A[rr][PAT_SCALARS];
                    for (c = rr + 1; c < PAT_SCALARS; c++) s -= A[rr][c] * d[c];
                    d[rr] = s / A[rr][rr];
                }
                for (i = 0; i < PAT_SCALARS; i++) sw[i] += d[i];
                for (k = 0; k < n; k++) {
                    const float *f = sf + k * PAT_SCALARS;
                    for (i = 0; i < PAT_SCALARS; i++) r[k] -= d[i] * f[i];
                }
            }
            for (k = 0; k < n; k++) { if (val[k]) vse += r[k] * r[k]; else se += r[k] * r[k]; }
            mse = se / (double)(n - nval);
            vmse = nval ? vse / (double)nval : 0;
            /* stop once the held-out error has stopped improving (more passes only fit noise) */
            if (vmse < best * 0.9995) { best = vmse; worse = 0; } else worse++;
            if (it % 10 == 0)
                fprintf(stderr, "  phase %d it %3d  train rmse %.3f  valid rmse %.3f\n", ph, it, sqrt(mse), sqrt(vmse));
        }
        fprintf(stderr, "phase %d: %zu positions, %d iterations, rmse train %.2f valid %.2f discs, scalars %.2f %.3f %.3f %.3f\n",
                ph, n, it, sqrt(mse), sqrt(vmse), sw[0], sw[1], sw[2], sw[3]);
        {
            int *q = qall + (size_t)ph * (entries + PAT_SCALARS);
            for (i = 0; i < PAT_SCALARS; i++) q[i] = (int)lround(sw[i] * Q);
            for (i = 0; i < entries; i++) q[PAT_SCALARS + i] = (int)lround(w[i] * Q);
        }
        W[ph] = w;
        memcpy(SW[ph], sw, sizeof sw);
        free(g); free(cnt); free(ix); free(sf); free(tg); free(val); free(r);
    }

#if OTH_PAT_SET == 2
    {   /* microcontroller level: plain const arrays (they stay in flash), 8-bit patterns */
        const int PQ = 8, SQ = 256;            /* must match PW_Q / PS_Q in engine.c */
        long clipped = 0;
        int j;
        out = fopen(argv[1], "w");
        if (!out) return 1;
        fprintf(out, "/* Microcontroller pattern weights - generated by tools/train/train.c (OTH_PAT_SET=2) from %zu\n"
                     " * self-play positions. %d phases x %d pattern entries (1/%d disc, int8) + %d scalars (1/%d disc).\n"
                     " * Used by engine.c with OTH_PATTERNS=2. Do not edit by hand. */\n"
                     "#include <stdint.h>\n\nconst int16_t oth_wmcu_sc[%d] = {\n",
                nrec, PAT_PHASES, entries, PQ, PAT_SCALARS, SQ, PAT_PHASES * PAT_SCALARS);
        for (ph = 0; ph < PAT_PHASES; ph++) {
            fprintf(out, "   ");
            for (i = 0; i < PAT_SCALARS; i++) fprintf(out, " %ld,", lround(SW[ph][i] * SQ));
            fprintf(out, "\n");
        }
        fprintf(out, "};\n\nconst int8_t oth_wmcu[%d] = {\n", PAT_PHASES * entries);
        for (ph = 0; ph < PAT_PHASES; ph++)
            for (j = 0; j < entries; j++) {
                long v = lround(W[ph][j] * PQ);
                if (v > 127) { v = 127; clipped++; }
                if (v < -127) { v = -127; clipped++; }
                fprintf(out, "%ld,%s", v, (j % 32 == 31 || j == entries - 1) ? "\n" : "");
            }
        fputs("};\n", out);
        fclose(out);
        fprintf(stderr, "wrote %s (%d bytes of tables, %ld weights clipped)\n", argv[1], PAT_PHASES * entries, clipped);
        return 0;
    }
#endif
    encode(qall, PAT_PHASES * (entries + PAT_SCALARS));
    out = fopen(argv[1], "w");
    if (!out) return 1;
    fprintf(out, "/* Pattern evaluation weights - generated by tools/train/train.c from %zu self-play positions.\n"
                 " * %d phases x (%d scalars + %d pattern entries), 1/%d disc units, zero-run + zigzag varint.\n"
                 " * Decoded by oth_init() in engine.c. Do not edit by hand. */\n"
                 "#include <stdint.h>\n\nconst uint32_t oth_wdata_len = %zuu;\nconst unsigned char oth_wdata[] =\n\"",
            nrec, PAT_PHASES, PAT_SCALARS, entries, (int)Q, on);
    {
        size_t col = 0, j;
        for (j = 0; j < on; j++) {
            unsigned c = ob[j];
            if (col > 110) { fputs("\"\n\"", out); col = 0; }
            if (c >= 32 && c < 127 && c != '"' && c != '\\' && c != '?') { fputc((int)c, out); col++; }
            else { fprintf(out, "\\%03o", c); col += 4; }   /* 3-digit octal never swallows the next char */
        }
    }
    fputs("\";\n", out);
    fclose(out);
    fprintf(stderr, "wrote %s (%zu bytes of data)\n", argv[1], on);
    return 0;
}
