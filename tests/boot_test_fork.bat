@echo off
REM ARC OS fork/wait QEMU gate: base shell expectations plus forktest proof.
setlocal
cd /d "%~dp0.."

echo ============================================
echo   ARC OS FORK QEMU GATE
echo ============================================
echo.

python tools\boot_test.py --timeout 30 --settle 1 --expect "arc> " ^
    --expect "ARC OS Commands:" --expect "runtime.txt" ^
    --expect "Process exited" --expect "beta" ^
    --expect-not "alphabeta" --expect-not "betaa" ^
    --expect "fork: child ok" --expect "fork: wait ok" ^
    --input "help" --input "clear" ^
    --input "touch runtime.txt" --input "ls" ^
    --input "echo alpha to fdtest.txt" ^
    --input "echo beta to fdtest.txt" --input "cat fdtest.txt" ^
    --input "rm fdtest.txt" --input "forktest" --input "exit"
if errorlevel 1 (
    echo FORK QEMU GATE FAILED
    exit /b 1
)
echo.
echo FORK QEMU GATE PASSED
exit /b 0
