@echo off
REM Clean rebuild of ARC OS (removes all cached build objects first).
setlocal
cd /d "%~dp0"

echo === ARC OS Clean Rebuild ===
python tools\build_kernel.py --clean %*
if errorlevel 1 (
    echo.
    echo BUILD FAILED
    exit /b 1
)

echo.
echo Build complete: %CD%\os.elf
exit /b 0
