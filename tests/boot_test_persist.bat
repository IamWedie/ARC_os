@echo off
setlocal
cd /d "%~dp0.."
echo ============================================
echo   ARC OS PERSIST QEMU GATE
echo ============================================
python tests\boot_test_persist.py %*
if errorlevel 1 (
  echo PERSIST QEMU GATE FAILED
  exit /b 1
)
echo PERSIST QEMU GATE PASSED
exit /b 0
