@echo off
echo === TinyOS Run ===
echo Starting QEMU...

REM Check if QEMU is available
where qemu-system-x86_64 >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo ERROR: qemu-system-x86_64 not found in PATH
    echo Install QEMU from https://www.qemu.org/
    echo Or set QEMU path manually
    goto :eof
)

REM Boot the OS
qemu-system-x86_64 -kernel os.elf -nographic -m 512M

echo.
echo QEMU exited.
