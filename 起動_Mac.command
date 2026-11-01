#!/bin/bash
# オセロを起動します（この窓を閉じると終了します）
echo "オセロを起動します（この窓を閉じると終了します）"
exec bash "$(dirname "$0")/mac/start.sh"
