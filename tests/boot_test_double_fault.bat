@echo off
REM ARC OS deliberate double-fault / IST QEMU gate. Always restores the normal kernel.
setlocal
cd /d "%~dp0.."

echo ============================================
echo   ARC OS DOUBLE-FAULT IST QEMU GATE
echo ============================================
echo.

set "GATE_RC=1"
set "ARC_EXCEPTION_TEST="
set "ARC_RING0_FAULT_TEST="
set "ARC_PANIC_DIAG_TEST="
set "ARC_DOUBLE_FAULT_TEST=1"
python tools\build_kernel.py --quiet
if errorlevel 1 goto restore
python tests\boot_test_double_fault.py
set "GATE_RC=%ERRORLEVEL%"

:restore
set "ARC_DOUBLE_FAULT_TEST="
set "ARC_PANIC_DIAG_TEST="
python tools\build_kernel.py --quiet
set "RESTORE_RC=%ERRORLEVEL%"
if not "%RESTORE_RC%"=="0" exit /b %RESTORE_RC%
if not "%GATE_RC%"=="0" (
    echo DOUBLE-FAULT IST QEMU GATE FAILED
    exit /b %GATE_RC%
)
echo.
echo DOUBLE-FAULT IST QEMU GATE PASSED
exit /b 0
