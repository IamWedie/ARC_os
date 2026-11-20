@echo off
REM ARC OS full build: 64-bit kernel + ring-3 shell + multiboot boot image.
setlocal
cd /d "%~dp0"

echo === ARC OS Full Build ===
python tools\build_kernel.py %*
if errorlevel 1 (
    echo.
    echo BUILD FAILED
    exit /b 1
)

echo.
echo Build complete: %CD%\os.elf
exit /b 0
