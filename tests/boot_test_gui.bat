@echo off
REM ARC OS M1 GUI QEMU gate: screendump pixel assertions.
setlocal
cd /d "%~dp0.."

echo ============================================
echo   ARC OS GUI QEMU GATE
echo ============================================
echo.

python tests\boot_test_gui.py
if errorlevel 1 (
    echo GUI QEMU GATE FAILED
    exit /b 1
)
echo.
echo GUI QEMU GATE PASSED
exit /b 0
