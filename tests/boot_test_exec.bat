@echo off
REM ARC OS exec QEMU gate: base shell expectations plus fork -> exec -> wait proof.
setlocal
cd /d "%~dp0.."

echo ============================================
echo   ARC OS EXEC QEMU GATE
echo ============================================
echo.

python tools\boot_test.py --timeout 30 --settle 1 --expect "arc> " ^
    --expect "ARC OS Commands:" --expect "runtime.txt" ^
    --expect "Process exited" --expect "beta" ^
    --expect-not "alphabeta" --expect-not "betaa" ^
    --expect "exec: hello ok" --expect "exec: wait ok" ^
    --input "help" --input "clear" ^
    --input "touch runtime.txt" --input "ls" ^
    --input "echo alpha to fdtest.txt" ^
    --input "echo beta to fdtest.txt" --input "cat fdtest.txt" ^
    --input "rm fdtest.txt" --input "forktest" --input "execdemo" --input "exit"
if errorlevel 1 (
    echo EXEC QEMU GATE FAILED
    exit /b 1
)
echo.
echo EXEC QEMU GATE PASSED
exit /b 0
