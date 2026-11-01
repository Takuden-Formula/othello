@echo off
cd /d "%~dp0"
echo オセロを起動します（この黒い窓を閉じると終了します）
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0serve.ps1"
if errorlevel 1 (
  echo.
  echo 起動に失敗しました。server_log.txt を確認してください。
)
pause
