/*
 * Othello engine - see engine.h
 *
 * Strength comes from:
 *   - bitboard move generation (fast on 32-bit MCUs too)
 *   - iterative deepening PVS with a transposition table
 *   - move ordering (hash move, shallow eval, fastest-first in the endgame)
 *   - evaluation: mobility, potential mobility, corners, X/C squares, stable discs, parity
 *   - perfect endgame: once depth >= empties the search plays the game out exactly
 */
#include "engine.h"
#include <stdlib.h>
#include <string.h>

#ifndef OTH_TT_BITS
# if defined(ARDUINO)
#  define OTH_TT_BITS 13          /* 8192 * 24 B = 192 KB, halved automatically if malloc fails */
# else
#  define OTH_TT_BITS 21          /* 2M * 24 B = 48 MB (PC / Raspberry Pi) */
# endif
#endif

#define INF 30000

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

static int evaluate(uint64_t P, uint64_t O)
{
    uint64_t E = ~(P | O), mP = oth_moves(P, O), mO = oth_moves(O, P);
    int ne = pc(E), s = 0, k;
    int wMob, wPot, wFront, wStab, wDisc;

    if (!mP && !mO) return final_score(P, O);

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

uint32_t oth_init(void)
{
    int bits = OTH_TT_BITS;
    init_diagonals();
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

/* Exact solver for the last few empties (no hash table, cheap ordering). */
static int solve_small(uint64_t P, uint64_t O, int alpha, int beta, int passed)
{
    uint64_t moves = oth_moves(P, O), order[2];
    int best = -INF, i;
    nodes++;
    if (!moves) {
        if (passed) return final_score(P, O);
        return -solve_small(O, P, -beta, -alpha, 1);
    }
    order[0] = moves & CORNERS;
    order[1] = moves & ~CORNERS;
    for (i = 0; i < 2; i++) {
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

    /* generate + score moves */
    while (moves) {
        int sq = lsb(moves), key;
        uint64_t b = 1ULL << sq, f = oth_flips(P, O, sq);
        uint64_t cp = O & ~f, co = P | f | b;
        moves &= moves - 1;
        if (sq == hashmove) key = 1 << 28;
        else if (depth >= ne) {
            /* endgame: fastest first (leave the opponent few replies) */
            key = -pc(oth_moves(cp, co)) * 64 + ((b & CORNERS) ? 48 : 0) - pc(f);
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
        /* the next iteration would most likely not finish */
        if ((uint32_t)(oth_millis() - t_start) * 3 > t_limit && d >= 4) break;
    }
    r.nodes = nodes;
    r.ms = oth_millis() - t_start;
    return r;
}

/* ---------------------------------------------------------------- perft */

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
