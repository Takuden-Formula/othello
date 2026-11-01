/*
 * Othello for Raspberry Pi Pico (RP2040) - play over the serial monitor (115200 bps, line ending = LF or CR+LF)
 *
 * Board: "Raspberry Pi Pico" from the arduino-pico core (Earle Philhower), FQBN rp2040:rp2040:rpipico.
 * engine.c / engine.h / patterns.h in this folder are the same engine as the PC version, built at the
 * "microcontroller level" (OTH_PATTERNS=2, automatic on Arduino): a learned pattern evaluation stored as
 * 8-bit tables in weights_mcu.c (~370 KB), read straight from flash, so it needs almost no RAM.
 * The hash table is 4096 entries (96 KB) on the Pico (RAM is 264 KB).
 * The on-board LED is on while the AI is thinking.
 *
 * The search is recursive and needs up to ~20 KB of stack (about 770 bytes per ply), but the Arduino
 * stack of core 0 is only 4-8 KB (scratch RAM). So the AI runs on core 1 with its own 40 KB stack,
 * and core 0 keeps the serial console.
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
#include "pico/multicore.h"

extern "C" uint32_t oth_millis(void) { return millis(); }

struct Pos { Board b; bool blackToMove; };

static Pos ps;
static Pos hist[130];
static int nh = 0;
static bool humanBlack = true, humanWhite = false;
static uint32_t thinkMs = 5000;
static String line;

/* ---- AI on core 1 (own big stack) */
#define AI_STACK_WORDS (40 * 1024 / 4)
static uint32_t aiStack[AI_STACK_WORDS];
static volatile bool aiBusy = false;
static Board aiBoard;
static uint32_t aiMs;
static SearchResult aiResult;

static void aiCore()
{
    for (;;) {
        if (aiBusy) {
            __dmb();
            aiResult = oth_search(&aiBoard, aiMs, 60);
            __dmb();
            aiBusy = false;
        }
        tight_loop_contents();
    }
}

static SearchResult think(const Board &b)
{
    aiBoard = b;
    aiMs = thinkMs;
    __dmb();
    aiBusy = true;
    digitalWrite(LED_BUILTIN, HIGH);
    while (aiBusy) delay(1);
    __dmb();
    digitalWrite(LED_BUILTIN, LOW);
    return aiResult;
}

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
        SearchResult r = think(ps.b);
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
        SearchResult r = think(ps.b);
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
    pinMode(LED_BUILTIN, OUTPUT);
    /* USB serial: wait up to 10 s for the serial monitor so the first board is not missed */
    while (!Serial && millis() < 10000) delay(10);
    delay(200);
    uint32_t entries = oth_init();
    multicore_launch_core1_with_stack(aiCore, aiStack, sizeof aiStack);
    Serial.printf("\nOthello Raspberry Pi Pico  hash entries=%u  free heap=%d  CPU %lu MHz\n",
                  (unsigned)entries, rp2040.getFreeHeap(), (unsigned long)(rp2040.f_cpu() / 1000000));
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
