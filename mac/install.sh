#!/bin/bash
# オセロ - Mac にダウンロードして展開する（ターミナルに1行貼るだけ）
#   curl -fsSL https://raw.githubusercontent.com/Takuden-Formula/othello/main/mac/install.sh | bash
# ブラウザでダウンロードしたものと違い「開発元を検証できません」の警告が出ない（curl は隔離の印を付けないため）
set -e
URL="https://github.com/Takuden-Formula/othello/archive/refs/heads/main.zip"   # ブランチの最新
DEST="${1:-$HOME/Desktop}"
[ -d "$DEST" ] || DEST="$HOME"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/othello.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

echo "ダウンロードしています…（1 MB 未満）"
curl -fL --progress-bar -o "$TMP/a.zip" "$URL"
ditto -x -k "$TMP/a.zip" "$TMP/x"
SRC="$(find "$TMP/x" -maxdepth 1 -type d -name 'othello-*' | head -1)"   # ZIP の中身は othello-main/ フォルダ
[ -n "$SRC" ] && [ -f "$SRC/docs/index.html" ] || { echo "ZIP の中にオセロのフォルダがありません"; exit 1; }
if [ -e "$DEST/othello" ]; then
  OLD="$DEST/othello_old_$(date +%Y%m%d_%H%M%S)"
  mv "$DEST/othello" "$OLD"
  echo "前の版は $OLD に残しました（記録 records.csv を置いていたら移してください）"
fi
ditto "$SRC" "$DEST/othello"
xattr -dr com.apple.quarantine "$DEST/othello" 2>/dev/null || true
chmod +x "$DEST/othello/"*.command "$DEST/othello/mac/"*.sh "$DEST/othello/mac/"*.pl "$DEST/othello/mac/"*.command 2>/dev/null || true
echo
echo "できました: $DEST/othello"
echo "開いたフォルダの「起動_Mac.command」をダブルクリックするとゲームが始まります"
open "$DEST/othello"
