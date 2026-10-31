/*
 * Othello engine (bitboard, alpha-beta / PVS, transposition table, perfect endgame)
 * Portable C99. Runs on PC, Raspberry Pi, ESP32 (Arduino).
 *
 * Board layout: bit index = row*8 + col, a1 = bit 0, h1 = bit 7, a8 = bit 56.
 * Board.p = discs of the side to move, Board.o = discs of the opponent.
 */
#ifndef OTHELLO_ENGINE_H
#define OTHELLO_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTH_PASS     (-1)
#define OTH_SCALE    100     /* evaluation units per disc */
#define OTH_WIN      10000   /* bonus added to proven wins */
#define OTH_EVAL_MAX 9000    /* heuristic scores are clamped to +-this */

typedef struct { uint64_t p, o; } Board;

typedef struct {
    int      move;   /* 0..63, or OTH_PASS */
    int      score;  /* side-to-move view, OTH_SCALE per disc (+-OTH_WIN when proven) */
    int      depth;  /* last fully searched depth */
    int      exact;  /* 1 = perfect play result (endgame solved to the end) */
    uint64_t nodes;
    uint32_t ms;
} SearchResult;

/* Must be provided by the platform (millis() on Arduino, clock_gettime on Linux). */
uint32_t oth_millis(void);

/* Allocate the hash table. Returns number of entries actually allocated. */
uint32_t oth_init(void);
void     oth_new_game(void);                    /* clears the hash table */

void     oth_start(Board *b);                   /* initial position, black to move */
uint64_t oth_moves(uint64_t p, uint64_t o);
uint64_t oth_flips(uint64_t p, uint64_t o, int sq);
void     oth_play(Board *b, int sq);            /* sq or OTH_PASS; side to move swaps */
int      oth_count(uint64_t x);
int      oth_evaluate(const Board *b);

/* Search: stops after time_ms, or at max_depth (use 60 for "as deep as time allows"). */
SearchResult oth_search(const Board *b, uint32_t time_ms, int max_depth);

/* Converts a score to discs (for display). Sets *proven when the result is a proven win/loss/draw. */
int      oth_score_to_discs(int score, int *proven);

uint64_t oth_perft(uint64_t p, uint64_t o, int depth, int passed);

#ifdef __cplusplus
}
#endif

#endif
