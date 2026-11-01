/*
 * Self-play data generator for training the pattern evaluation.
 *   selfplay <games> <seed> <depth> <out.bin>
 * Each game: a few random opening moves, then depth-limited search (with a small chance of a
 * random move for variety); from ENDGAME_EMPTIES empties on both sides play perfectly, so the
 * final disc difference is the exact value of the game from that point.
 * Every position (side to move P, opponent O) is written with the final disc difference seen
 * from P: 8 bytes P, 8 bytes O, 1 byte signed score.
 */
#include "../../src/engine.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
uint32_t oth_millis(void) { return (uint32_t)GetTickCount(); }
#else
#include <time.h>
uint32_t oth_millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}
#endif

#define ENDGAME_EMPTIES 14

static uint64_t rng;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

static int random_move(uint64_t m)
{
    int n = oth_count(m), k = (int)(rnd() % (uint32_t)n);
    while (k--) m &= m - 1;
    return __builtin_ctzll(m);
}

int main(int argc, char **argv)
{
    int games, depth, g;
    FILE *out;
    if (argc < 5) { fprintf(stderr, "usage: selfplay games seed depth out.bin\n"); return 1; }
    games = atoi(argv[1]);
    rng = 0x9E3779B97F4A7C15ULL ^ (uint64_t)atoll(argv[2]) * 0x2545F4914F6CDD1DULL;
    if (!rng) rng = 1;
    depth = atoi(argv[3]);
    out = fopen(argv[4], "wb");
    if (!out) return 1;
    oth_init();
    for (g = 0; g < games; g++) {
        Board b, hist[80];
        int side[80], nh = 0, black = 1, ply = 0, nrand = 2 + (int)(rnd() % 9), i, diff;
        oth_start(&b);
        oth_new_game();
        for (;;) {
            uint64_t m = oth_moves(b.p, b.o);
            int ne = 64 - oth_count(b.p | b.o), sq;
            if (!m) {
                if (!oth_moves(b.o, b.p)) break;
                oth_play(&b, OTH_PASS); black ^= 1;
                continue;
            }
            hist[nh] = b; side[nh] = black; nh++;
            if (ply < nrand || (ne > ENDGAME_EMPTIES && rnd() % 100 < 4)) sq = random_move(m);
            else if (ne <= ENDGAME_EMPTIES) sq = oth_search(&b, 600000, 60).move;
            else sq = oth_search(&b, 600000, depth).move;
            oth_play(&b, sq); black ^= 1; ply++;
        }
        /* b.p is the side to move at the end; turn it into black's disc difference */
        diff = oth_count(b.p) - oth_count(b.o);
        {
            int e = 64 - oth_count(b.p | b.o);
            if (diff > 0) diff += e; else if (diff < 0) diff -= e;
        }
        if (!black) diff = -diff;
        for (i = 0; i < nh; i++) {
            signed char s = (signed char)(side[i] ? diff : -diff);
            fwrite(&hist[i].p, 8, 1, out);
            fwrite(&hist[i].o, 8, 1, out);
            fwrite(&s, 1, 1, out);
        }
        if ((g + 1) % 1000 == 0) { fprintf(stderr, "%d\n", g + 1); fflush(out); }
    }
    fclose(out);
    return 0;
}
