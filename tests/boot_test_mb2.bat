@echo off
setlocal
cd /d "%~dp0.."
echo ============================================
echo   ARC OS MB2 QEMU GATE
echo ============================================
python tests\boot_test_mb2.py %*
if errorlevel 1 (
  echo MB2 QEMU GATE FAILED
  exit /b 1
)
echo MB2 QEMU GATE PASSED
exit /b 0