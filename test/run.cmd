@echo off
rem Build and run the host self-test (no hardware needed). Usage: test\run.cmd
rem Keep this file ASCII-only: cmd.exe decodes batch files with the OEM code page (936 here).
pushd "%~dp0.."

gcc -std=c99 -O2 -Wall -Wextra ^
    -I. -Imodels -Itest ^
    test\port_host.c test\dice_smoke.c ^
    Dice.c models\D20.c models\D6.c models\Poly.c ^
    models\D4.c models\D8.c models\D10.c models\D12.c ^
    -lm -o dice_smoke_test.exe
if errorlevel 1 (popd & exit /b 1)

dice_smoke_test.exe
set RC=%ERRORLEVEL%
popd
exit /b %RC%
