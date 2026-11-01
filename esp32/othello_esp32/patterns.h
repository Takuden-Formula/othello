/*
 * Pattern evaluation shared by the engine (engine.c) and the trainer (tools/train/train.c).
 *
 * The board is cut into 46 lines/blocks ("pattern instances"): 4 edges + 2 X squares, 4 corner 3x3,
 * 8 corner 2x5, rows/columns 2-4, and the diagonals of length 4-8. Every instance of the same shape
 * shares one weight table, indexed by the base-3 number of its squares (0 empty, 1 side to move,
 * 2 opponent). Weights are learned per game phase (number of empties) from self-play games.
 */
#ifndef OTHELLO_PATTERNS_H
#define OTHELLO_PATTERNS_H

#include <stdint.h>

/* Two shape sets: OTH_PAT_SET 1 = PC (large tables, decoded into RAM), 2 = microcontroller (small
 * tables read straight from flash: no shape longer than 9 squares, fewer phases). */
#ifndef OTH_PAT_SET
# define OTH_PAT_SET 1
#endif

#define PAT_MAXLEN  10
#define PAT_SCALARS 4             /* bias, mobility, potential mobility, parity */

#if OTH_PAT_SET == 2
#define PAT_TYPES   11
#define PAT_PHASES  6
static const signed char PAT_BASE[PAT_TYPES][PAT_MAXLEN + 1] = {
    /* squares as row*8+col, -1 terminated; the first element of each shape is its "corner" end */
    {  0,  1,  2,  3,  4,  5,  6,  7, -1 },           /* edge                  3^8  */
    {  0,  1,  2,  8,  9, 10, 16, 17, 18, -1 },       /* corner 3x3            3^9  */
    {  0,  1,  2,  3,  8,  9, 10, 11, -1 },           /* corner 2x4            3^8  */
    {  8,  9, 10, 11, 12, 13, 14, 15, -1 },           /* row 2                 3^8  */
    { 16, 17, 18, 19, 20, 21, 22, 23, -1 },           /* row 3                 3^8  */
    { 24, 25, 26, 27, 28, 29, 30, 31, -1 },           /* row 4                 3^8  */
    {  0,  9, 18, 27, 36, 45, 54, 63, -1 },           /* diagonal 8            3^8  */
    {  1, 10, 19, 28, 37, 46, 55, -1 },               /* diagonal 7            3^7  */
    {  2, 11, 20, 29, 38, 47, -1 },                   /* diagonal 6            3^6  */
    {  3, 12, 21, 30, 39, -1 },                       /* diagonal 5            3^5  */
    {  4, 13, 22, 31, -1 },                           /* diagonal 4            3^4  */
};
#else
#define PAT_TYPES   11
#ifndef PAT_PHASES
# define PAT_PHASES 10
#endif
static const signed char PAT_BASE[PAT_TYPES][PAT_MAXLEN + 1] = {
    /* squares as row*8+col, -1 terminated; the first element of each shape is its "corner" end */
    {  0,  1,  2,  3,  4,  5,  6,  7,  9, 14, -1 },   /* edge + 2 X squares   3^10 */
    {  0,  1,  2,  8,  9, 10, 16, 17, 18, -1 },       /* corner 3x3            3^9  */
    {  0,  1,  2,  3,  4,  8,  9, 10, 11, 12, -1 },   /* corner 2x5            3^10 */
    {  8,  9, 10, 11, 12, 13, 14, 15, -1 },           /* row 2                 3^8  */
    { 16, 17, 18, 19, 20, 21, 22, 23, -1 },           /* row 3                 3^8  */
    { 24, 25, 26, 27, 28, 29, 30, 31, -1 },           /* row 4                 3^8  */
    {  0,  9, 18, 27, 36, 45, 54, 63, -1 },           /* diagonal 8            3^8  */
    {  1, 10, 19, 28, 37, 46, 55, -1 },               /* diagonal 7            3^7  */
    {  2, 11, 20, 29, 38, 47, -1 },                   /* diagonal 6            3^6  */
    {  3, 12, 21, 30, 39, -1 },                       /* diagonal 5            3^5  */
    {  4, 13, 22, 31, -1 },                           /* diagonal 4            3^4  */
};
#endif

typedef struct {
    int     n;                    /* number of instances */
    uint8_t type[64];
    uint8_t len[64];
    uint8_t sq[64][PAT_MAXLEN];
    int     size[PAT_TYPES];      /* 3^len of each shape */
    int     offset[PAT_TYPES + 1];/* start of each shape's table inside one phase */
} PatSet;

static int pat_sym(int sq, int s)
{
    int r = sq >> 3, c = sq & 7, t;
    if (s & 1) c = 7 - c;
    if (s & 2) r = 7 - r;
    if (s & 4) { t = r; r = c; c = t; }
    return r * 8 + c;
}

/* Builds the instances (46 in both sets): the 8 symmetries of every shape, keeping one per distinct square set. */
static void pat_build(PatSet *ps)
{
    int t, s, i, j, off = 0;
    uint64_t seen[64];
    ps->n = 0;
    for (t = 0; t < PAT_TYPES; t++) {
        int len = 0, p3 = 1;
        while (len < PAT_MAXLEN && PAT_BASE[t][len] >= 0) len++;
        for (i = 0; i < len; i++) p3 *= 3;
        ps->size[t] = p3;
        ps->offset[t] = off;
        off += p3;
        for (s = 0; s < 8; s++) {
            uint64_t m = 0;
            int dup = 0;
            for (i = 0; i < len; i++) m |= 1ULL << pat_sym(PAT_BASE[t][i], s);
            for (j = 0; j < ps->n; j++) if (seen[j] == m) dup = 1;
            if (dup) continue;
            seen[ps->n] = m;
            ps->type[ps->n] = (uint8_t)t;
            ps->len[ps->n] = (uint8_t)len;
            for (i = 0; i < len; i++) ps->sq[ps->n][i] = (uint8_t)pat_sym(PAT_BASE[t][i], s);
            ps->n++;
        }
    }
    ps->offset[PAT_TYPES] = off;
}

static inline int pat_phase(int empties)
{
    int ph = (60 - empties) * PAT_PHASES / 61;
    return ph < 0 ? 0 : ph >= PAT_PHASES ? PAT_PHASES - 1 : ph;
}

/* Fills idx[] with the weight index (inside one phase) of every instance. */
static inline void pat_indices(const PatSet *ps, uint64_t P, uint64_t O, uint32_t *idx)
{
    int k, i;
    for (k = 0; k < ps->n; k++) {
        uint32_t x = 0;
        for (i = ps->len[k] - 1; i >= 0; i--) {
            int sq = ps->sq[k][i];
            x = x * 3 + (uint32_t)((P >> sq & 1) | (O >> sq & 1) << 1);
        }
        idx[k] = (uint32_t)ps->offset[ps->type[k]] + x;
    }
}

#endif
