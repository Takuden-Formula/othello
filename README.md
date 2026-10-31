# オセロ（最強AI）

ビットボード＋αβ探索（PVS）＋置換表で読み、終盤は最後まで読み切る C 言語のオセロ AI。
紅陵祭2026 電気自動車同好会の展示用。

## いちばん簡単な遊び方（Windows・Mac・iPhone・Android）
**https://takuden-formula.github.io/othello/** を開くだけ（インストール不要）。
C 言語の AI をそのまま WebAssembly にしてブラウザで動かしている。この URL を送れば誰でも遊べる。

## Windows（ターミナル版）
`windows/othello.exe` をダブルクリック。遊び方は `windows/遊び方.txt`。

## Mac（ターミナル版）
1. このページの緑の「Code」→「Download ZIP」で落として展開する
2. `mac/オセロ_Mac.command` をダブルクリック（初回だけ自動でビルドする。C コンパイラが無ければ macOS がインストールを案内する）
   - 「開発元を検証できないため開けません」と出たら、**システム設定 → プライバシーとセキュリティ** の下の方で「このまま開く」を押す
   - それでも開けないときは、ターミナルで `bash ` と打って（最後に空白）`オセロ_Mac.command` をドラッグして return
3. `1`（黒）/`2`（白）/`3`（AI どうし）を選び、`d3` のように打つ。`undo` = 待った、`hint` = ヒント、`quit` = 終了

思考時間を変えるときは、ターミナルで `bash mac/オセロ_Mac.command -t 10`（1 手 10 秒）。

## フォルダ
| パス | 中身 |
|---|---|
| `src/` | AI 本体 `engine.c` / `engine.h`、ターミナル版 `main.c`、`Makefile`（Linux / ラズパイは `make`）、ブラウザ版の橋渡し `wasm_api.c` |
| `docs/` | ブラウザ版（GitHub Pages で公開）。`index.html`・`worker.js`・`othello.wasm` |
| `esp32/othello_esp32/` | ESP32 用 Arduino スケッチ（シリアルモニタ 115200bps で遊ぶ） |
| `windows/` | Windows 用 exe |
| `mac/` | Mac 用の起動ファイル |
| `tools/build_wasm.sh` | `othello.wasm` を作り直す（`pip install ziglang` が必要） |

## AI を直したとき
- `src/engine.c` を直したら `esp32/othello_esp32/` にもコピーする
- ブラウザ版：`bash tools/build_wasm.sh` → `docs/othello.wasm` をコミットして push（1〜2分で反映）
- Windows 版：MinGW gcc で `gcc -O3 -march=x86-64-v2 -static -s -o othello.exe main.c engine.c`（`src/` で）
- Mac 版：次に `オセロ_Mac.command` を開いたときに自動でビルドし直す
