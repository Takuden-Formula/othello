/*
 * Othello - console game (PC / Raspberry Pi)
 *
 *   ./othello                 you = black, AI thinks 3 s per move
 *   ./othello -c w -t 10      you = white, AI thinks 10 s per move
 *   ./othello -c none         AI vs AI
 *   ./othello -perft 9        move generator check
 *   ./othello -match 20 -weak 2   test: strong AI vs depth-2 AI
 */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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

typedef struct { Board b; int black_to_move; } Pos;

static void sq_name(int sq, char *out)
{
    if (sq == OTH_PASS) { strcpy(out, "pass"); return; }
    out[0] = (char)('a' + (sq & 7));
    out[1] = (char)('1' + (sq >> 3));
    out[2] = 0;
}

static int parse_sq(const char *s)
{
    int c, r;
    while (*s == ' ') s++;
    c = tolower((unsigned char)s[0]) - 'a';
    r = s[1] - '1';
    if (c < 0 || c > 7 || r < 0 || r > 7) return -2;
    return r * 8 + c;
}

static void print_board(const Pos *ps, int show_moves)
{
    uint64_t black = ps->black_to_move ? ps->b.p : ps->b.o;
    uint64_t white = ps->black_to_move ? ps->b.o : ps->b.p;
    uint64_t mv = show_moves ? oth_moves(ps->b.p, ps->b.o) : 0;
    int r, c;
    printf("\n    a b c d e f g h\n");
    for (r = 0; r < 8; r++) {
        printf("  %d", r + 1);
        for (c = 0; c < 8; c++) {
            int sq = r * 8 + c;
            char ch = '.';
            if (black >> sq & 1) ch = 'X';
            else if (white >> sq & 1) ch = 'O';
            else if (mv >> sq & 1) ch = '*';
            printf(" %c", ch);
        }
        printf("\n");
    }
    printf("  X(黒)=%d  O(白)=%d   手番: %s\n",
           oth_count(black), oth_count(white), ps->black_to_move ? "X(黒)" : "O(白)");
}

static void print_result(const SearchResult *r)
{
    char m[8];
    int proven, d = oth_score_to_discs(r->score, &proven);
    sq_name(r->move, m);
    printf("  AI: %s  評価 %+d石%s  深さ %d  %llu ノード  %.1f 秒  (%.0f kN/s)\n",
           m, d, r->exact ? " (完全読み)" : proven ? " (勝敗確定)" : "",
           r->depth, (unsigned long long)r->nodes, r->ms / 1000.0,
           r->ms ? (double)r->nodes / r->ms : 0.0);
}

static void play_move(Pos *ps, int sq)
{
    oth_play(&ps->b, sq);
    ps->black_to_move = !ps->black_to_move;
}

static int game_over(const Pos *ps)
{
    return !oth_moves(ps->b.p, ps->b.o) && !oth_moves(ps->b.o, ps->b.p);
}

/* Returns 1 when the game was played to the end, 0 on quit / end of input. */
static int game(int human_black, int human_white, uint32_t tms)
{
    Pos hist[130], ps;
    int nh = 0;
    char line[128];

    oth_start(&ps.b);
    ps.black_to_move = 1;
    oth_new_game();
    printf("コマンド: d3 のように打つ / undo = 待った / hint = ヒント / quit = 終了\n");

    for (;;) {
        int human = ps.black_to_move ? human_black : human_white;
        print_board(&ps, human);
        if (game_over(&ps)) break;
        if (!oth_moves(ps.b.p, ps.b.o)) {
            printf("  %s はパス\n", ps.black_to_move ? "X(黒)" : "O(白)");
            hist[nh++] = ps;
            play_move(&ps, OTH_PASS);
            continue;
        }
        if (human) {
            int sq;
            printf("> ");
            fflush(stdout);
            if (!fgets(line, sizeof line, stdin)) return 0;
            line[strcspn(line, "\r\n")] = 0;
            if (!strcmp(line, "quit") || !strcmp(line, "q")) return 0;
            if (!strcmp(line, "undo") || !strcmp(line, "u")) {
                /* go back to the previous position where a human was to move */
                while (nh > 0) {
                    int was_human;
                    ps = hist[--nh];
                    was_human = ps.black_to_move ? human_black : human_white;
                    if (was_human && oth_moves(ps.b.p, ps.b.o)) break;
                }
                continue;
            }
            if (!strcmp(line, "hint") || !strcmp(line, "h")) {
                SearchResult r = oth_search(&ps.b, tms, 60);
                print_result(&r);
                continue;
            }
            sq = parse_sq(line);
            if (sq < 0 || !(oth_moves(ps.b.p, ps.b.o) >> sq & 1)) {
                printf("  そこには打てません（* が打てる場所）\n");
                continue;
            }
            hist[nh++] = ps;
            play_move(&ps, sq);
        } else {
            SearchResult r;
            printf("  考え中...\n");
            fflush(stdout);
            r = oth_search(&ps.b, tms, 60);
            print_result(&r);
            hist[nh++] = ps;
            play_move(&ps, r.move);
        }
    }
    {
        uint64_t black = ps.black_to_move ? ps.b.p : ps.b.o;
        uint64_t white = ps.black_to_move ? ps.b.o : ps.b.p;
        int nb = oth_count(black), nw = oth_count(white);
        printf("\n終局  X(黒) %d - %d O(白)  → %s\n", nb, nw,
               nb > nw ? "黒の勝ち" : nb < nw ? "白の勝ち" : "引き分け");
    }
    return 1;
}

/* Menu shown when started without -c (e.g. double-clicked on Windows). */
static int ask_line(const char *prompt, char *buf, int n)
{
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, n, stdin)) return 0;
    buf[strcspn(buf, "\r\n")] = 0;
    return 1;
}

static int choose_side(int *human_black, int *human_white)
{
    char buf[32];
    printf("\n  1 : 自分が黒 X（先手）\n  2 : 自分が白 O（後手）\n  3 : AI どうし\n");
    if (!ask_line("番号を入力して Enter > ", buf, sizeof buf)) return 0;
    *human_black = buf[0] != '2' && buf[0] != '3';
    *human_white = buf[0] == '2';
    return 1;
}

/* Strength test: strong AI (time tms) vs weak AI (fixed depth), random 4-ply openings,
   each opening played with both colours. */
static void match(int games, int weak_depth, uint32_t tms)
{
    int w = 0, l = 0, dr = 0, g;
    long disc_sum = 0;
    srand(12345);
    for (g = 0; g < games; g++) {
        Pos ps;
        int strong_black = (g & 1) == 0, k;
        if ((g & 1) == 0) srand(1000 + g);
        else srand(1000 + g - 1);
        oth_start(&ps.b);
        ps.black_to_move = 1;
        oth_new_game();
        for (k = 0; k < 4; k++) {
            uint64_t mv = oth_moves(ps.b.p, ps.b.o);
            int cnt = oth_count(mv), pick = rand() % cnt;
            while (pick--) mv &= mv - 1;
            play_move(&ps, __builtin_ctzll(mv));
        }
        while (!game_over(&ps)) {
            int strong = ps.black_to_move == strong_black;
            SearchResult r;
            if (!oth_moves(ps.b.p, ps.b.o)) { play_move(&ps, OTH_PASS); continue; }
            r = strong ? oth_search(&ps.b, tms, 60) : oth_search(&ps.b, 600000, weak_depth);
            play_move(&ps, r.move);
        }
        {
            uint64_t black = ps.black_to_move ? ps.b.p : ps.b.o;
            uint64_t white = ps.black_to_move ? ps.b.o : ps.b.p;
            int d = oth_count(black) - oth_count(white);
            if (!strong_black) d = -d;
            disc_sum += d;
            if (d > 0) w++; else if (d < 0) l++; else dr++;
            printf("game %2d: strong=%s  %+d\n", g + 1, strong_black ? "black" : "white", d);
            fflush(stdout);
        }
    }
    printf("strong vs depth-%d: %d win / %d loss / %d draw, avg %+.1f discs\n",
           weak_depth, w, l, dr, (double)disc_sum / games);
}

int main(int argc, char **argv)
{
    int human_black = 1, human_white = 0, i;
    uint32_t tms = 3000;
    uint32_t entries;
    int match_games = 0, weak = 2, side_given = 0;

#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
    entries = oth_init();

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            tms = (uint32_t)(atof(argv[++i]) * 1000);
        } else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            const char *c = argv[++i];
            side_given = 1;
            human_black = (c[0] == 'b');
            human_white = (c[0] == 'w');
            if (!strcmp(c, "both")) human_black = human_white = 1;
        } else if (!strcmp(argv[i], "-perft") && i + 1 < argc) {
            Board b;
            int d, n = atoi(argv[++i]);
            oth_start(&b);
            for (d = 1; d <= n; d++) {
                uint32_t t0 = oth_millis();
                uint64_t c = oth_perft(b.p, b.o, d, 0);
                printf("perft %2d: %llu  (%u ms)\n", d, (unsigned long long)c, oth_millis() - t0);
            }
            return 0;
        } else if (!strcmp(argv[i], "-match") && i + 1 < argc) {
            match_games = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-weak") && i + 1 < argc) {
            weak = atoi(argv[++i]);
        } else {
            printf("usage: %s [-c b|w|none|both] [-t sec] [-perft n] [-match n [-weak d]]\n", argv[0]);
            return 1;
        }
    }
    if (match_games > 0) { match(match_games, weak, tms); return 0; }
    printf("オセロ  (置換表 %u エントリ, 思考時間 %.1f 秒)\n", entries, tms / 1000.0);
    for (;;) {
        char buf[32];
        if (!side_given && !choose_side(&human_black, &human_white)) break;
        if (!game(human_black, human_white, tms)) break;
        if (!ask_line("\nもう一度遊ぶ？ (y = はい / Enter = 終了) > ", buf, sizeof buf)) break;
        if (buf[0] != 'y' && buf[0] != 'Y') break;
    }
    return 0;
}
