#!/bin/bash
# オセロを全画面で起動します
echo "オセロを全画面で起動します"
echo "最初に画面をクリックすると全画面になります。Esc で全画面解除、F で再び全画面"
exec bash "$(dirname "$0")/mac/start.sh" --kiosk
