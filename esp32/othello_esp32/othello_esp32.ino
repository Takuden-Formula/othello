/*
 * Othello for ESP32 - play over the serial monitor (115200 bps, line ending = LF or CR+LF)
 *
 * engine.c / engine.h / patterns.h in this folder are the same engine as the PC / Raspberry Pi
 * version, built at the "microcontroller level" (OTH_PATTERNS=2): a smaller learned pattern
 * evaluation (weights_mcu.c, ~370 KB of 8-bit tables) that is read straight from flash, so it needs
 * almost no RAM. The PC level tables (weights.c) would need ~3 MB of RAM.
 *
 * Commands:
 *   d3        play a move
 *   new b     new game, you play black (X, moves first)
 *   new w     new game, you play white (O)
 *   auto      AI vs AI
 *   t 5       AI thinking time in seconds
 *   hint      ask the AI for the best move
 *   undo      take back your last move
 */
#include "engine.h"

extern "C" uint32_t oth_millis(void) { return millis(); }

/* The search is recursive (about 800 bytes of stack per ply, 20+ plies in the endgame); the default
 * loop() stack of 8 KB is not enough. */
SET_LOOP_TASK_STACK_SIZE(32 * 1024);

struct Pos { Board b; bool blackToMove; };

static Pos ps;
static Pos hist[130];
static int nh = 0;
static bool humanBlack = true, humanWhite = false;
static uint32_t thinkMs = 5000;
static String line;

static void sqName(int sq, char *out)
{
    if (sq == OTH_PASS) { strcpy(out, "pass"); return; }
    out[0] = 'a' + (sq & 7);
    out[1] = '1' + (sq >> 3);
    out[2] = 0;
}

static bool isHumanTurn() { return ps.blackToMove ? humanBlack : humanWhite; }

static bool gameOver() { return !oth_moves(ps.b.p, ps.b.o) && !oth_moves(ps.b.o, ps.b.p); }

static void printBoard()
{
    uint64_t black = ps.blackToMove ? ps.b.p : ps.b.o;
    uint64_t white = ps.blackToMove ? ps.b.o : ps.b.p;
    uint64_t mv = isHumanTurn() ? oth_moves(ps.b.p, ps.b.o) : 0;
    Serial.println();
    Serial.println("    a b c d e f g h");
    for (int r = 0; r < 8; r++) {
        Serial.printf("  %d", r + 1);
        for (int c = 0; c < 8; c++) {
            int sq = r * 8 + c;
            char ch = '.';
            if (black >> sq & 1) ch = 'X';
            else if (white >> sq & 1) ch = 'O';
            else if (mv >> sq & 1) ch = '*';
            Serial.printf(" %c", ch);
        }
        Serial.println();
    }
    Serial.printf("  X(black)=%d  O(white)=%d   to move: %s\n",
                  oth_count(black), oth_count(white), ps.blackToMove ? "X" : "O");
}

static void printResult(const SearchResult &r)
{
    char m[8];
    int proven, d = oth_score_to_discs(r.score, &proven);
    sqName(r.move, m);
    Serial.printf("  AI: %s  eval %+d%s  depth %d  %llu nodes  %.1f s\n", m, d,
                  r.exact ? " (perfect)" : proven ? " (proven)" : "",
                  r.depth, (unsigned long long)r.nodes, r.ms / 1000.0);
}

static void playMove(int sq)
{
    hist[nh++] = ps;
    oth_play(&ps.b, sq);
    ps.blackToMove = !ps.blackToMove;
}

static void newGame(bool hb, bool hw)
{
    humanBlack = hb;
    humanWhite = hw;
    oth_start(&ps.b);
    ps.blackToMove = true;
    nh = 0;
    oth_new_game();
    Serial.printf("\nnew game  you: %s  AI time: %.1f s\n",
                  hb && hw ? "both" : hb ? "X (black)" : hw ? "O (white)" : "none (AI vs AI)",
                  thinkMs / 1000.0);
}

/* Lets the AI move (and handles passes) until it is a human's turn or the game ends. */
static void advance()
{
    for (;;) {
        printBoard();
        if (gameOver()) {
            uint64_t black = ps.blackToMove ? ps.b.p : ps.b.o;
            uint64_t white = ps.blackToMove ? ps.b.o : ps.b.p;
            int nb = oth_count(black), nw = oth_count(white);
            Serial.printf("\nGAME OVER  X %d - %d O  -> %s\n", nb, nw,
                          nb > nw ? "black wins" : nb < nw ? "white wins" : "draw");
            Serial.println("type 'new b' or 'new w' to play again");
            return;
        }
        if (!oth_moves(ps.b.p, ps.b.o)) {
            Serial.printf("  %s passes\n", ps.blackToMove ? "X" : "O");
            playMove(OTH_PASS);
            continue;
        }
        if (isHumanTurn()) { Serial.print("> "); return; }
        Serial.println("  thinking...");
        SearchResult r = oth_search(&ps.b, thinkMs, 60);
        printResult(r);
        playMove(r.move);
    }
}

static void handle(String cmd)
{
    cmd.trim();
    cmd.toLowerCase();
    if (cmd.length() == 0) return;

    if (cmd.startsWith("new")) {
        char c = cmd.length() > 4 ? cmd[4] : 'b';
        newGame(c == 'b', c == 'w');
    } else if (cmd == "auto") {
        newGame(false, false);
    } else if (cmd.startsWith("t ")) {
        float s = cmd.substring(2).toFloat();
        if (s > 0) thinkMs = (uint32_t)(s * 1000);
        Serial.printf("AI time = %.1f s\n> ", thinkMs / 1000.0);
        return;
    } else if (cmd == "hint") {
        SearchResult r = oth_search(&ps.b, thinkMs, 60);
        printResult(r);
        Serial.print("> ");
        return;
    } else if (cmd == "undo") {
        while (nh > 0) {
            ps = hist[--nh];
            if (isHumanTurn() && oth_moves(ps.b.p, ps.b.o)) break;
        }
    } else {
        int c = cmd[0] - 'a', r = cmd.length() > 1 ? cmd[1] - '1' : -1;
        if (c < 0 || c > 7 || r < 0 || r > 7) {
            Serial.println("  ? commands: d3 / new b / new w / auto / t 5 / hint / undo");
            Serial.print("> ");
            return;
        }
        int sq = r * 8 + c;
        if (!isHumanTurn() || !(oth_moves(ps.b.p, ps.b.o) >> sq & 1)) {
            Serial.println("  illegal move (* = legal)");
            Serial.print("> ");
            return;
        }
        playMove(sq);
    }
    advance();
}

void setup()
{
    Serial.begin(115200);
    delay(500);
    uint32_t entries = oth_init();
    Serial.printf("\nOthello ESP32  hash entries=%u  free heap=%u\n", entries, ESP.getFreeHeap());
    Serial.println("commands: d3 / new b / new w / auto / t 5 / hint / undo");
    newGame(true, false);
    advance();
}

void loop()
{
    while (Serial.available()) {
        char ch = Serial.read();
        if (ch == '\n' || ch == '\r') {
            if (line.length()) { handle(line); line = ""; }
        } else {
            line += ch;
        }
    }
}
