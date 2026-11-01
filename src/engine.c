/*
 * Othello engine - see engine.h
 *
 * Strength comes from:
 *   - bitboard move generation (fast on 32-bit MCUs too)
 *   - iterative deepening PVS with a transposition table
 *   - move ordering (hash move, shallow eval, fastest-first in the endgame)
 *   - evaluation: pattern weights learned from self-play games (patterns.h, tools/train/).
 *     OTH_PATTERNS 1 = PC level: large tables (weights.c), decoded into ~3 MB of RAM.
 *     OTH_PATTERNS 2 = microcontroller level: small 8-bit tables (weights_mcu.c, ~370 KB) read
 *     straight from flash, so it needs almost no RAM (default on Arduino / ESP32).
 *     OTH_PATTERNS 0 = the old hand-written one: mobility, potential mobility, corners, X/C squares,
 *     stable discs, parity
 *   - perfect endgame: once depth >= empties the search plays the game out exactly
 *     (parity ordering for the last empties, jump to the exact search near the end)
 */
#include "engine.h"
#include <stdlib.h>
#include <string.h>

#ifndef OTH_PATTERNS
# if defined(ARDUINO)
#  define OTH_PATTERNS 2
# else
#  define OTH_PATTERNS 1
# endif
#endif
#if OTH_PATTERNS
# define OTH_PAT_SET OTH_PATTERNS
# include "patterns.h"
#endif

#ifndef OTH_TT_BITS
# if defined(ARDUINO_ARCH_RP2040)
#  define OTH_TT_BITS 12          /* Raspberry Pi Pico: 4096 * 24 B = 96 KB of its 264 KB RAM */
# elif defined(ARDUINO)
#  define OTH_TT_BITS 13         /* 8192 * 24 B = 192 KB, halved automatically if malloc fails */
# else
#  define OTH_TT_BITS 22          /* 4M * 24 B = 96 MB (PC / Raspberry Pi) */
# endif
#endif

#define INF 30000

#ifndef OTH_ENDGAME_JUMP
# define OTH_ENDGAME_JUMP 8      /* go straight to the exact search once depth + this >= empties */
#endif

static const uint64_t NOT_A   = 0xFEFEFEFEFEFEFEFEULL;
static const uint64_t NOT_H   = 0x7F7F7F7F7F7F7F7FULL;
static const uint64_t INNER_H = 0x7E7E7E7E7E7E7E7EULL;
static const uint64_t COL_A   = 0x0101010101010101ULL;
static const uint64_t COL_H   = 0x8080808080808080ULL;
static const uint64_t ROW_1   = 0x00000000000000FFULL;
static const uint64_t ROW_8   = 0xFF00000000000000ULL;
static const uint64_t CORNERS = 0x8100000000000081ULL;
static const uint64_t BORDER  = 0xFF818181818181FFULL;

/* ---------------------------------------------------------------- basics */

static inline int pc(uint64_t x)
{
    return __builtin_popcount((uint32_t)x) + __builtin_popcount((uint32_t)(x >> 32));
}

static inline int lsb(uint64_t x)
{
    uint32_t lo = (uint32_t)x;
    return lo ? __builtin_ctz(lo) : 32 + __builtin_ctz((uint32_t)(x >> 32));
}

int oth_count(uint64_t x) { return pc(x); }

#define SL1(x) ((x) << 1)
#define SR1(x) ((x) >> 1)
#define SL7(x) ((x) << 7)
#define SR7(x) ((x) >> 7)
#define SL8(x) ((x) << 8)
#define SR8(x) ((x) >> 8)
#define SL9(x) ((x) << 9)
#define SR9(x) ((x) >> 9)

uint64_t oth_moves(uint64_t P, uint64_t O)
{
    uint64_t mO = O & INNER_H, t, moves = 0;
#define MDIR(SH, M) \
    t = (M) & SH(P); t |= (M) & SH(t); t |= (M) & SH(t); \
    t |= (M) & SH(t); t |= (M) & SH(t); t |= (M) & SH(t); moves |= SH(t);
    MDIR(SL1, mO) MDIR(SR1, mO)
    MDIR(SL8, O)  MDIR(SR8, O)
    MDIR(SL7, mO) MDIR(SR7, mO)
    MDIR(SL9, mO) MDIR(SR9, mO)
#undef MDIR
    return moves & ~(P | O);
}

uint64_t oth_flips(uint64_t P, uint64_t O, int sq)
{
    uint64_t m = 1ULL << sq, mO = O & INNER_H, f = 0, x, t;
#define FDIR(SH, M) \
    x = SH(m); t = 0; while (x & (M)) { t |= x; x = SH(x); } if (x & P) f |= t;
    FDIR(SL1, mO) FDIR(SR1, mO)
    FDIR(SL8, O)  FDIR(SR8, O)
    FDIR(SL7, mO) FDIR(SR7, mO)
    FDIR(SL9, mO) FDIR(SR9, mO)
#undef FDIR
    return f;
}

void oth_start(Board *b)
{
    /* black: d5 e4, white: d4 e5, black to move */
    b->p = (1ULL << 35) | (1ULL << 28);
    b->o = (1ULL << 27) | (1ULL << 36);
}

void oth_play(Board *b, int sq)
{
    uint64_t p = b->p, o = b->o;
    if (sq != OTH_PASS) {
        uint64_t f = oth_flips(p, o, sq);
        p |= f | (1ULL << sq);
        o &= ~f;
    }
    b->p = o;
    b->o = p;
}

static inline uint64_t neighbours(uint64_t x)
{
    uint64_t h = ((x << 1) & NOT_A) | ((x >> 1) & NOT_H);
    uint64_t r = x | h;
    return (r << 8) | (r >> 8) | h;
}

/* ---------------------------------------------------------------- scoring */

static int final_score(uint64_t P, uint64_t O)
{
    int p = pc(P), o = pc(O), e = 64 - p - o, d = p - o;
    if (d > 0) return OTH_WIN + (d + e) * OTH_SCALE;
    if (d < 0) return -OTH_WIN + (d - e) * OTH_SCALE;
    return 0;
}

int oth_score_to_discs(int score, int *proven)
{
    int pr = score > OTH_EVAL_MAX || score < -OTH_EVAL_MAX;
    if (proven) *proven = pr;
    if (score > OTH_EVAL_MAX)  score -= OTH_WIN;
    if (score < -OTH_EVAL_MAX) score += OTH_WIN;
    return score >= 0 ? (score + OTH_SCALE / 2) / OTH_SCALE : -((-score + OTH_SCALE / 2) / OTH_SCALE);
}

static uint64_t diag7[15], diag9[15];

static void init_diagonals(void)
{
    int sq;
    memset(diag7, 0, sizeof diag7);
    memset(diag9, 0, sizeof diag9);
    for (sq = 0; sq < 64; sq++) {
        int r = sq >> 3, c = sq & 7;
        diag9[r - c + 7] |= 1ULL << sq;   /* a1-h8 direction */
        diag7[r + c]     |= 1ULL << sq;   /* h1-a8 direction */
    }
}

static uint64_t full_lines(uint64_t filled, uint64_t *fh, uint64_t *fv, uint64_t *f7, uint64_t *f9)
{
    uint64_t h = 0, v = 0, d7 = 0, d9 = 0;
    int i;
    for (i = 0; i < 8; i++) {
        uint64_t rm = ROW_1 << (8 * i), cm = COL_A << i;
        if ((filled & rm) == rm) h |= rm;
        if ((filled & cm) == cm) v |= cm;
    }
    for (i = 0; i < 15; i++) {
        if ((filled & diag7[i]) == diag7[i]) d7 |= diag7[i];
        if ((filled & diag9[i]) == diag9[i]) d9 |= diag9[i];
    }
    *fh = h; *fv = v; *f7 = d7; *f9 = d9;
    return h & v & d7 & d9;
}

/* Discs of P that can never be flipped (conservative). */
static uint64_t stable_discs(uint64_t P, uint64_t fh, uint64_t fv, uint64_t f7, uint64_t f9)
{
    uint64_t hb = fh | COL_A | COL_H, vb = fv | ROW_1 | ROW_8;
    uint64_t b7 = f7 | BORDER, b9 = f9 | BORDER;
    uint64_t st = 0, old;
    do {
        uint64_t h, v, d7, d9;
        old = st;
        h  = hb | ((st << 1) & NOT_A) | ((st >> 1) & NOT_H);
        v  = vb | (st << 8) | (st >> 8);
        d9 = b9 | ((st << 9) & NOT_A) | ((st >> 9) & NOT_H);
        d7 = b7 | ((st << 7) & NOT_H) | ((st >> 7) & NOT_A);
        st = P & h & v & d7 & d9;
    } while (st != old);
    return st;
}

static const struct { int c, x, c1, c2; } CORNER_INFO[4] = {
    { 0,  9,  1,  8 }, { 7, 14,  6, 15 }, { 56, 49, 57, 48 }, { 63, 54, 62, 55 }
};

#if OTH_PATTERNS
static PatSet pset;
static int pstride, pready;

#if OTH_PATTERNS == 2
/* ---- microcontroller level: per phase, 8-bit pattern tables (1/PW_Q disc) and 16-bit scalars
 * (1/PS_Q disc), both const so they stay in flash */
#define PW_Q 8
#define PS_Q 256
extern const int8_t  oth_wmcu[];
extern const int16_t oth_wmcu_sc[];

static int load_weights(void)
{
    if (!pready) { pat_build(&pset); pstride = pset.offset[PAT_TYPES]; pready = 1; }
    return 1;
}

static int pattern_eval(uint64_t P, uint64_t O, uint64_t mP, uint64_t mO)
{
    uint64_t E = ~(P | O);
    int ne = pc(E), ph = pat_phase(ne), k, i;
    const int8_t  *t = oth_wmcu + (size_t)ph * pstride;
    const int16_t *w = oth_wmcu_sc + ph * PAT_SCALARS;
    int32_t s = w[0]
        + w[1] * (pc(mP) - pc(mO))
        + w[2] * (pc(neighbours(O) & E) - pc(neighbours(P) & E))
        + w[3] * ((ne & 1) ? 1 : -1), sp = 0;
    for (k = 0; k < pset.n; k++) {
        uint32_t x = 0;
        for (i = pset.len[k] - 1; i >= 0; i--) {
            int sq = pset.sq[k][i];
            x = x * 3 + (uint32_t)((P >> sq & 1) | (O >> sq & 1) << 1);
        }
        sp += t[pset.offset[pset.type[k]] + x];
    }
    s = (s + sp * (PS_Q / PW_Q)) * OTH_SCALE / PS_Q;
    if (s > OTH_EVAL_MAX)  s = OTH_EVAL_MAX;
    if (s < -OTH_EVAL_MAX) s = -OTH_EVAL_MAX;
    return (int)s;
}
#else
/* ---- PC level: weights in 1/16 disc, per phase: 4 scalars then the pattern tables */
extern const uint32_t oth_wdata_len;
extern const unsigned char oth_wdata[];
static int16_t *pweights;

static int load_weights(void)
{
    const unsigned char *d = oth_wdata, *end = oth_wdata + oth_wdata_len;
    int n, i = 0;
    if (pweights) return 1;
    pat_build(&pset);
    pstride = PAT_SCALARS + pset.offset[PAT_TYPES];
    n = PAT_PHASES * pstride;
    pweights = (int16_t *)calloc((size_t)n, sizeof(int16_t));
    if (!pweights) return 0;
    while (d < end && i < n) {
        unsigned v = 0, sh = 0, zero = (*d == 0);
        if (zero) d++;
        do { v |= (unsigned)(*d & 127) << sh; sh += 7; } while (*d++ & 128);
        if (zero) i += (int)v;
        else pweights[i++] = (int16_t)((v & 1) ? (int)((v + 1) / 2) : -(int)(v / 2));
    }
    pready = 1;
    return 1;
}

static int pattern_eval(uint64_t P, uint64_t O, uint64_t mP, uint64_t mO)
{
    uint64_t E = ~(P | O);
    int ne = pc(E), k, i;
    const int16_t *w = pweights + (size_t)pat_phase(ne) * pstride;
    const int16_t *t = w + PAT_SCALARS;
    int32_t s = w[0]
        + w[1] * (pc(mP) - pc(mO))
        + w[2] * (pc(neighbours(O) & E) - pc(neighbours(P) & E))
        + w[3] * ((ne & 1) ? 1 : -1);
    for (k = 0; k < pset.n; k++) {
        uint32_t x = 0;
        for (i = pset.len[k] - 1; i >= 0; i--) {
            int sq = pset.sq[k][i];
            x = x * 3 + (uint32_t)((P >> sq & 1) | (O >> sq & 1) << 1);
        }
        s += t[pset.offset[pset.type[k]] + x];
    }
    s = s * OTH_SCALE / 16;
    if (s > OTH_EVAL_MAX)  s = OTH_EVAL_MAX;
    if (s < -OTH_EVAL_MAX) s = -OTH_EVAL_MAX;
    return (int)s;
}
#endif
#endif

static int evaluate(uint64_t P, uint64_t O)
{
    uint64_t E = ~(P | O), mP = oth_moves(P, O), mO = oth_moves(O, P);
    int ne = pc(E), s = 0, k;
    int wMob, wPot, wFront, wStab, wDisc;

    if (!mP && !mO) return final_score(P, O);
#if OTH_PATTERNS
    if (pready) return pattern_eval(P, O, mP, mO);
#endif

    if (ne > 40)      { wMob = 90; wPot = 25; wFront = 30; wStab = 120; wDisc = -4; }
    else if (ne > 20) { wMob = 80; wPot = 20; wFront = 25; wStab = 150; wDisc = 0;  }
    else              { wMob = 60; wPot = 10; wFront = 15; wStab = 170; wDisc = 20; }

    /* mobility (corner moves count triple) */
    s += wMob * ((pc(mP) + 2 * pc(mP & CORNERS)) - (pc(mO) + 2 * pc(mO & CORNERS)));
    /* potential mobility: empties next to opponent discs */
    s += wPot * (pc(neighbours(O) & E) - pc(neighbours(P) & E));
    /* frontier discs (touching empties) are bad */
    s -= wFront * (pc(P & neighbours(E)) - pc(O & neighbours(E)));
    /* corners */
    s += 800 * (pc(P & CORNERS) - pc(O & CORNERS));
    /* X / C squares next to an empty corner */
    for (k = 0; k < 4; k++) {
        if (!(E >> CORNER_INFO[k].c & 1)) continue;
        s -= 350 * ((int)(P >> CORNER_INFO[k].x & 1) - (int)(O >> CORNER_INFO[k].x & 1));
        s -= 120 * ((int)(P >> CORNER_INFO[k].c1 & 1) + (int)(P >> CORNER_INFO[k].c2 & 1)
                  - (int)(O >> CORNER_INFO[k].c1 & 1) - (int)(O >> CORNER_INFO[k].c2 & 1));
    }
    /* stable discs (only worth computing once something is anchored) */
    if ((P | O) & CORNERS) {
        uint64_t fh, fv, f7, f9;
        full_lines(P | O, &fh, &fv, &f7, &f9);
        s += wStab * (pc(stable_discs(P, fh, fv, f7, f9)) - pc(stable_discs(O, fh, fv, f7, f9)));
    }
    /* parity: with an odd number of empties the side to move gets the last move */
    if (ne <= 24) s += (ne & 1) ? 60 : -60;
    s += wDisc * (pc(P) - pc(O));

    if (s > OTH_EVAL_MAX)  s = OTH_EVAL_MAX;
    if (s < -OTH_EVAL_MAX) s = -OTH_EVAL_MAX;
    return s;
}

int oth_evaluate(const Board *b) { return evaluate(b->p, b->o); }

/* ---------------------------------------------------------------- hash table */

typedef struct {
    uint64_t p, o;
    int16_t  lo, hi;
    int8_t   depth;
    int8_t   move;
    uint8_t  gen;
    uint8_t  pad;
} TTEntry;

static TTEntry *tt;
static uint32_t tt_mask;
static uint8_t  tt_gen;

static inline uint32_t hash_pos(uint64_t P, uint64_t O)
{
    uint64_t h = P * 0x9E3779B97F4A7C15ULL ^ (O + 0x632BE59BD9B4E019ULL) * 0xC2B2AE3D27D4EB4FULL;
    h ^= h >> 31;
    return (uint32_t)(h ^ (h >> 32)) & tt_mask;
}

uint32_t oth_init(void) { return oth_init_tt(OTH_TT_BITS); }

uint32_t oth_init_tt(int bits)
{
    init_diagonals();
#if OTH_PATTERNS
    load_weights();
#endif
    if (tt) return tt_mask + 1;
    while (bits >= 8) {
        tt = (TTEntry *)calloc((size_t)1 << bits, sizeof(TTEntry));
        if (tt) break;
        bits--;
    }
    tt_mask = tt ? ((1u << bits) - 1) : 0;
    return tt ? tt_mask + 1 : 0;
}

void oth_new_game(void)
{
    if (tt) memset(tt, 0, sizeof(TTEntry) * (tt_mask + 1));
    tt_gen = 0;
}

static inline TTEntry *tt_probe(uint64_t P, uint64_t O)
{
    TTEntry *e;
    if (!tt) return 0;
    e = &tt[hash_pos(P, O)];
    return (e->p == P && e->o == O && (e->p | e->o)) ? e : 0;
}

static inline void tt_store(uint64_t P, uint64_t O, int depth, int alpha, int beta, int score, int move)
{
    TTEntry *e;
    if (!tt) return;
    e = &tt[hash_pos(P, O)];
    if (e->gen == tt_gen && e->depth > depth && !(e->p == P && e->o == O)) return;
    e->p = P; e->o = O;
    e->depth = (int8_t)depth;
    e->move = (int8_t)move;
    e->gen = tt_gen;
    e->lo = (int16_t)(score > alpha ? score : -INF);
    e->hi = (int16_t)(score < beta ? score : INF);
}

/* ---------------------------------------------------------------- search */

static uint64_t nodes;
static uint32_t t_start, t_limit;
static int aborted;

/* ---- Multi-ProbCut: a shallow search predicts the deep one; if it is far outside the window,
 * cut without searching deeply. sigma = spread of (deep - shallow) in eval units, measured by
 * tools/train/mpc_calib.c per phase (empties / 6) and depth. Not used in exact endgame searches. */
#ifndef OTH_MPC
# define OTH_MPC (OTH_PATTERNS != 0) /* the sigmas were measured with the pattern evaluation */
#endif
#define MPC_MIN_DEPTH 3
#define MPC_MAX_DEPTH 14
#ifndef MPC_T10
# define MPC_T10      11         /* cut at 1.1 sigma (tested against 1.5 and 1.9) */
#endif
static int mpc_on = OTH_MPC;
static const int16_t MPC_SIGMA[11][MPC_MAX_DEPTH + 1] = {
#if OTH_PATTERNS == 2
#include "mpc_sigma_mcu.h"
#else
#include "mpc_sigma.h"
#endif
};
static inline int mpc_shallow(int d) { return d - 2 * ((d + 3) / 4); }

static const int8_t SQW[64] = {
    100, -20, 10,  5,  5, 10, -20, 100,
    -20, -50, -2, -2, -2, -2, -50, -20,
     10,  -2,  1,  1,  1,  1,  -2,  10,
      5,  -2,  1,  0,  0,  1,  -2,   5,
      5,  -2,  1,  0,  0,  1,  -2,   5,
     10,  -2,  1,  1,  1,  1,  -2,  10,
    -20, -50, -2, -2, -2, -2, -50, -20,
    100, -20, 10,  5,  5, 10, -20, 100,
};

typedef struct { uint64_t f; int sq; int key; } Move;

/* Empties in quadrants with an odd number of empties: playing there first usually lets the side to
 * move also take the last move of that region (parity). */
static inline uint64_t odd_regions(uint64_t E)
{
    static const uint64_t QUAD[4] = {
        0x000000000F0F0F0FULL, 0x00000000F0F0F0F0ULL, 0x0F0F0F0F00000000ULL, 0xF0F0F0F000000000ULL
    };
    uint64_t r = 0;
    int k;
    for (k = 0; k < 4; k++) if (pc(E & QUAD[k]) & 1) r |= QUAD[k];
    return r;
}

/* Exact solver for the last few empties (no hash table, cheap ordering). */
static int solve_small(uint64_t P, uint64_t O, int alpha, int beta, int passed)
{
    uint64_t moves = oth_moves(P, O), order[3], odd;
    int best = -INF, i;
    nodes++;
    if (!moves) {
        if (passed) return final_score(P, O);
        return -solve_small(O, P, -beta, -alpha, 1);
    }
    odd = odd_regions(~(P | O));
    order[0] = moves & odd & CORNERS;
    order[1] = moves & odd & ~CORNERS;
    order[2] = moves & ~odd;
    for (i = 0; i < 3; i++) {
        uint64_t m = order[i];
        while (m) {
            int sq = lsb(m), v;
            uint64_t b = 1ULL << sq, f = oth_flips(P, O, sq);
            m &= m - 1;
            v = -solve_small(O & ~f, P | f | b, -beta, -alpha, 0);
            if (v > best) {
                best = v;
                if (v > alpha) { alpha = v; if (alpha >= beta) return best; }
            }
        }
    }
    return best;
}

static int search(uint64_t P, uint64_t O, int depth, int alpha, int beta, int passed)
{
    uint64_t E = ~(P | O), moves;
    Move list[40];
    int ne = pc(E), n = 0, i, best = -INF, bestmove = -1, hashmove = -1, a0 = alpha;
    TTEntry *e;

    if ((++nodes & 1023) == 0 && (uint32_t)(oth_millis() - t_start) >= t_limit) aborted = 1;
    if (aborted) return 0;

    if (ne == 0) return final_score(P, O);
    if (depth >= ne && ne <= 6) return solve_small(P, O, alpha, beta, passed);
    if (depth <= 0) return evaluate(P, O);

    moves = oth_moves(P, O);
    if (!moves) {
        if (passed) return final_score(P, O);
        return -search(O, P, depth, -beta, -alpha, 1);
    }

    e = tt_probe(P, O);
    if (e) {
        hashmove = e->move;
        if (e->depth >= depth) {
            if (e->lo >= beta)  return e->lo;
            if (e->hi <= alpha) return e->hi;
            if (e->lo == e->hi) return e->lo;
        }
    }

    if (mpc_on && depth >= MPC_MIN_DEPTH && depth < ne && beta < OTH_EVAL_MAX && alpha > -OTH_EVAL_MAX) {
        int sig = MPC_SIGMA[ne / 6][depth > MPC_MAX_DEPTH ? MPC_MAX_DEPTH : depth] * MPC_T10 / 10;
        int ds = mpc_shallow(depth), bound, v;
        if (sig > 0) {
            bound = beta + sig;
            if (bound < OTH_EVAL_MAX) {
                v = search(P, O, ds, bound - 1, bound, passed);
                if (aborted) return 0;
                if (v >= bound) return beta;
            }
            bound = alpha - sig;
            if (bound > -OTH_EVAL_MAX) {
                v = search(P, O, ds, bound, bound + 1, passed);
                if (aborted) return 0;
                if (v <= bound) return alpha;
            }
        }
    }

    /* generate + score moves */
    while (moves) {
        int sq = lsb(moves), key;
        uint64_t b = 1ULL << sq, f = oth_flips(P, O, sq);
        uint64_t cp = O & ~f, co = P | f | b;
        moves &= moves - 1;
        if (sq == hashmove) key = 1 << 28;
        else if (depth >= ne) {
            /* endgame: fastest first (leave the opponent few replies), then parity */
            key = -pc(oth_moves(cp, co)) * 64 + ((b & CORNERS) ? 48 : 0) - pc(f)
                + ((b & odd_regions(E)) ? 20 : 0);
        } else if (depth >= 4) {
            key = -evaluate(cp, co);
        } else {
            key = SQW[sq] * 4 - pc(oth_moves(cp, co)) * 30;
        }
        list[n].f = f; list[n].sq = sq; list[n].key = key;
        n++;
    }

    for (i = 0; i < n; i++) {
        int j, bi = i, v;
        uint64_t b, f;
        for (j = i + 1; j < n; j++) if (list[j].key > list[bi].key) bi = j;
        if (bi != i) { Move t = list[i]; list[i] = list[bi]; list[bi] = t; }
        b = 1ULL << list[i].sq;
        f = list[i].f;
        if (i == 0) {
            v = -search(O & ~f, P | f | b, depth - 1, -beta, -alpha, 0);
        } else {
            v = -search(O & ~f, P | f | b, depth - 1, -alpha - 1, -alpha, 0);
            if (v > alpha && v < beta)
                v = -search(O & ~f, P | f | b, depth - 1, -beta, -alpha, 0);
        }
        if (aborted) return 0;
        if (v > best) {
            best = v; bestmove = list[i].sq;
            if (v > alpha) { alpha = v; if (alpha >= beta) break; }
        }
    }

    tt_store(P, O, depth, a0, beta, best, bestmove);
    return best;
}

SearchResult oth_search(const Board *bd, uint32_t time_ms, int max_depth)
{
    SearchResult r;
    Move root[40];
    uint64_t P = bd->p, O = bd->o, moves = oth_moves(P, O);
    int n = 0, ne = pc(~(P | O)), d, i;

    memset(&r, 0, sizeof r);
    r.move = OTH_PASS;
    nodes = 0;
    aborted = 0;
    t_start = oth_millis();
    t_limit = time_ms ? time_ms : 1;
    tt_gen++;

    if (!moves) {
        r.score = oth_moves(O, P) ? -evaluate(O, P) : final_score(P, O);
        return r;
    }
    while (moves) {
        int sq = lsb(moves);
        moves &= moves - 1;
        root[n].sq = sq;
        root[n].f = oth_flips(P, O, sq);
        root[n].key = SQW[sq];
        n++;
    }
    r.move = root[0].sq;
    if (n == 1) {
        Board t = *bd;
        oth_play(&t, r.move);
        r.score = -evaluate(t.p, t.o);
        r.ms = oth_millis() - t_start;
        return r;
    }
    if (max_depth > ne) max_depth = ne;
    if (max_depth < 1) max_depth = 1;

    for (d = 1; d <= max_depth; d++) {
        int alpha = -INF, beta = INF, bi = 0, done = 0;
        for (i = 0; i < n; i++) {
            uint64_t b = 1ULL << root[i].sq, f = root[i].f, cp = O & ~f, co = P | f | b;
            int v;
            if (i == 0) v = -search(cp, co, d - 1, -beta, -alpha, 0);
            else {
                v = -search(cp, co, d - 1, -alpha - 1, -alpha, 0);
                if (v > alpha && !aborted) v = -search(cp, co, d - 1, -beta, -alpha, 0);
            }
            if (aborted) break;
            done++;
            root[i].key = v;
            if (v > alpha) { alpha = v; bi = i; }
        }
        if (aborted) {
            /* the previous best was searched first; anything that beat it is trustworthy */
            if (done > 0 && bi != 0) { r.move = root[bi].sq; r.score = alpha; }
            break;
        }
        r.move = root[bi].sq;
        r.score = alpha;
        r.depth = d;
        /* best move first, others by score for the next iteration */
        {
            Move t = root[bi]; root[bi] = root[0]; root[0] = t;
            for (i = 1; i < n; i++) {
                int j;
                for (j = i + 1; j < n; j++)
                    if (root[j].key > root[i].key) { t = root[i]; root[i] = root[j]; root[j] = t; }
            }
        }
        if (d >= ne) { r.exact = 1; break; }
        /* near the end, the last few midgame depths cost about as much as solving: jump to it */
        if (ne <= 26 && d >= ne - OTH_ENDGAME_JUMP && d < ne - 1) d = ne - 1;
        /* the next iteration would most likely not finish */
        if ((uint32_t)(oth_millis() - t_start) * 3 > t_limit && d >= 4) break;
    }
    r.nodes = nodes;
    r.ms = oth_millis() - t_start;
    return r;
}

/* ---------------------------------------------------------------- perft */

void oth_set_mpc(int on) { mpc_on = on; }

uint64_t oth_perft(uint64_t P, uint64_t O, int depth, int passed)
{
    uint64_t moves, total = 0;
    if (depth == 0) return 1;
    moves = oth_moves(P, O);
    if (!moves) {
        if (passed) return 1;
        return oth_perft(O, P, depth - 1, 1);
    }
    while (moves) {
        int sq = lsb(moves);
        uint64_t b = 1ULL << sq, f = oth_flips(P, O, sq);
        moves &= moves - 1;
        total += oth_perft(O & ~f, P | f | b, depth - 1, 0);
    }
    return total;
}
