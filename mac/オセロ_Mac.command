#!/bin/bash
# オセロ（ターミナル版）Mac 用。初回は src/ の C 言語ソースからこの Mac 用にビルドしてから起動する。
# ブラウザで遊ぶだけなら README の URL を開く方が簡単（インストール不要）。
cd "$(dirname "$0")" || exit 1
pause(){ read -r -p "Enter キーで閉じます" _; }

if [ ! -x ./othello ] || [ ../src/engine.c -nt ./othello ] || [ ../src/main.c -nt ./othello ] || [ ../src/engine.h -nt ./othello ] || [ ../src/weights.c -nt ./othello ]; then
  # cc は Xcode コマンドラインツールに入っている。無いと macOS がインストールの案内を出す
  if ! xcrun --find cc >/dev/null 2>&1; then
    echo "C コンパイラがありません。表示されたインストールの案内に従って「コマンドライン・デベロッパ・ツール」を入れてから、もう一度開いてください。"
    xcode-select --install 2>/dev/null
    pause; exit 1
  fi
  echo "初回のビルド中…"
  if ! cc -O3 -o othello ../src/main.c ../src/engine.c ../src/weights.c; then
    echo "ビルドに失敗しました。Xcode を入れている場合は、ターミナルで sudo xcodebuild -license を実行して同意してから、もう一度開いてください。"
    pause; exit 1
  fi
fi

clear
./othello "$@"
pause
