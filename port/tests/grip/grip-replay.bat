@echo off
cd /d D:\claude\tlg_pc\tlg-windows-x86_64
set TLG_START_POINT=s02_TricosCave:_010_Care_Step1
set TLG_GRIP_AUTO=1
set TLG_GRIP_LOG=1
set TLG_GRIP_PASSIVE=1
set TLG_GRIP_REPLAY=1
set TLG_NO_TRICO_CAM=1
set TLG_PAD_REPLAY=D:\claude\run-jump.rec
set TLG_PAD_FILE=D:\claude\padfile.txt
set TLG_SKIP_LOGOS=1
set TLG_HDR=0
set TLG_FG=0
set TLG_FULLSCREEN=1
set TLG_FPS=unlocked
echo replay test begin %date% %time% > D:\claude\grip-jump.txt
for /L %%i in (1,1,5) do (
  type nul > D:\claude\padfile.txt
  echo run %%i start %time% >> D:\claude\grip-jump.txt
  start "" "Play The Last Guardian.exe"
  timeout /t 75 /nobreak > nul
  taskkill /F /IM tlg-probe.exe > nul 2>&1
  taskkill /F /IM "Play The Last Guardian.exe" > nul 2>&1
  timeout /t 5 /nobreak > nul
)
echo all done %time% >> D:\claude\grip-jump.txt
