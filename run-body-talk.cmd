@echo off
chcp 65001 >nul
title xiao-jingyu body (ears + mouth)
cd /d "%~dp0"
REM Double-click = open her ears and mouth.
REM Window open = she can hear the room and talk through the speaker.
REM Close the window (or Ctrl+C) = she stops listening. QQ keeps working.
REM
REM Audio now goes through a public MQTT broker, so the board no longer has to
REM be on the same LAN as this PC.
REM
REM Broker / topic prefix / port / TLS / account all come from ONE place:
REM     %USERPROFILE%\..\QQ bot\qq-bridge-deploy\state\body-config.json
REM (the console web page "身体" tab edits it; rot\tools\body_config.py reads it).
REM You normally do NOT pass --base / --broker. If you must override for a test:
REM     run-body-talk.cmd --base whale-1bb62897 --broker broker.hivemq.com
REM
REM Two knobs for how long she waits for you to finish a sentence:
REM   --silence 2.0   pause length that counts as "you finished" (raise it if she cuts you off)
REM   --max 25        hard cap on one utterance in seconds
"C:\Users\Administrator\Desktop\QQ bot\qq-bridge-deploy\tools\asr\.venv\Scripts\python.exe" tools\body_talk.py --loop --rounds 0 --wait 120 --silence 2.0 --max 25 %*
echo.
echo [stopped] body is no longer listening.
echo [stopped] press any key to close
pause >nul
