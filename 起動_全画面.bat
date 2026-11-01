@echo off
cd /d "%~dp0"
echo オセロを全画面で起動します
echo 最初に画面をクリックすると全画面になります。Esc で全画面解除、F で再び全画面
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0serve.ps1" -Kiosk
if errorlevel 1 (
  echo.
  echo 起動に失敗しました。server_log.txt を確認してください。
)
pause
