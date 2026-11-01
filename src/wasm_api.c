/*
 * WebAssembly wrapper for the browser version (docs/).
 * 64-bit boards are passed as two 32-bit halves so the JS side needs no BigInt.
 * Results are read back with the get_* functions.
 */
#include "engine.h"

#define EXPORT(name) __attribute__((export_name(name)))

__attribute__((import_module("env"), import_name("millis")))
extern uint32_t js_millis(void);

uint32_t oth_millis(void) { return js_millis(); }

static uint64_t ret64;
static SearchResult res;

static uint64_t mk(uint32_t lo, uint32_t hi) { return (uint64_t)hi << 32 | lo; }

/* bits = log2 of the hash table entries (24 B each), 0 = default (20: 24 MB) */
EXPORT("init")     uint32_t w_init(int bits) { return bits ? oth_init_tt(bits) : oth_init(); }
EXPORT("new_game") void     w_new_game(void) { oth_new_game(); }

EXPORT("moves") void w_moves(uint32_t plo, uint32_t phi, uint32_t olo, uint32_t ohi)
{
    ret64 = oth_moves(mk(plo, phi), mk(olo, ohi));
}

EXPORT("flips") void w_flips(uint32_t plo, uint32_t phi, uint32_t olo, uint32_t ohi, int sq)
{
    ret64 = oth_flips(mk(plo, phi), mk(olo, ohi), sq);
}

EXPORT("ret_lo") uint32_t w_ret_lo(void) { return (uint32_t)ret64; }
EXPORT("ret_hi") uint32_t w_ret_hi(void) { return (uint32_t)(ret64 >> 32); }

EXPORT("search") int w_search(uint32_t plo, uint32_t phi, uint32_t olo, uint32_t ohi, uint32_t time_ms)
{
    Board b;
    b.p = mk(plo, phi);
    b.o = mk(olo, ohi);
    res = oth_search(&b, time_ms, 60);
    return res.move;
}

EXPORT("get_score") int      w_score(void) { return res.score; }
EXPORT("get_depth") int      w_depth(void) { return res.depth; }
EXPORT("get_exact") int      w_exact(void) { return res.exact; }
EXPORT("get_nodes") double   w_nodes(void) { return (double)res.nodes; }
EXPORT("get_ms")    uint32_t w_ms(void)    { return res.ms; }

EXPORT("score_to_discs") int w_discs(int score)
{
    int proven;
    return oth_score_to_discs(score, &proven);
}
