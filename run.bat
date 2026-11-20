@echo off
echo === ARC OS Run ===
setlocal
cd /d "%~dp0"

if not exist os.elf (
    echo ERROR: os.elf not found. Run build.bat first.
    exit /b 1
)

set "QEMU=qemu-system-x86_64"
where qemu-system-x86_64 >nul 2>&1
if errorlevel 1 (
    if exist "%ProgramFiles%\qemu\qemu-system-x86_64.exe" (
        set "QEMU=%ProgramFiles%\qemu\qemu-system-x86_64.exe"
    ) else (
        echo ERROR: qemu-system-x86_64 not found.
        echo Install QEMU or add it to PATH.
        exit /b 1
    )
)

echo Starting QEMU... (Ctrl+A X to quit)
"%QEMU%" -kernel os.elf -nographic -m 512M
