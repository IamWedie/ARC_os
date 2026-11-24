@echo off
REM ARC OS SYS_READ/SYS_WRITE user-pointer QEMU gate. Always restores the normal kernel.
setlocal
cd /d "%~dp0.."

echo ============================================
echo   ARC OS SYSCALL POINTER QEMU GATE
echo ============================================
echo.

set "GATE_RC=1"
set "ARC_SYSCALL_TEST=1"
python tools\build_kernel.py --quiet
if errorlevel 1 goto restore
python tests\boot_test_syscall_ptr.py
set "GATE_RC=%ERRORLEVEL%"

:restore
set "ARC_SYSCALL_TEST="
python tools\build_kernel.py --quiet
set "RESTORE_RC=%ERRORLEVEL%"
if not "%RESTORE_RC%"=="0" exit /b %RESTORE_RC%
if not "%GATE_RC%"=="0" (
    echo SYSCALL POINTER QEMU GATE FAILED
    exit /b %GATE_RC%
)
echo.
echo SYSCALL POINTER QEMU GATE PASSED
exit /b 0
