@echo off
REM ARC OS quality gate: host memory test, full build, runtime boot test.
setlocal
cd /d "%~dp0.."

echo ============================================
echo   ARC OS QUALITY GATE
echo ============================================
echo.

echo [1/3] Host tests (slab/memory + scheduler)
cmd /c tests\run_tests.bat
if errorlevel 1 (
    echo QUALITY GATE FAILED: host tests
    exit /b 1
)
echo.

echo [2/3] Full kernel build
python tools\build_kernel.py --quiet
if errorlevel 1 (
    echo QUALITY GATE FAILED: build
    exit /b 1
)
echo.

echo [3/3] QEMU boot + shell command test
python tools\boot_test.py --timeout 30 --settle 1 --expect "arc> " ^
    --expect "ARC OS Commands:" --expect "runtime.txt" ^
    --expect "Process exited" --expect "beta" ^
    --expect-not "alphabeta" --input "help" --input "clear" ^
    --input "touch runtime.txt" --input "ls" ^
    --input "echo alpha to fdtest.txt" ^
    --input "echo beta to fdtest.txt" --input "cat fdtest.txt" ^
    --input "rm fdtest.txt" --input "exit"
if errorlevel 1 (
    echo QUALITY GATE FAILED: boot test
    exit /b 1
)
echo.

echo QUALITY GATE PASSED
exit /b 0
