@echo off
setlocal
cd /d "%~dp0.."
echo ============================================
echo   ARC OS PERSIST AHCI QEMU GATE
echo ============================================
python tests\boot_test_persist_ahci.py %*
if errorlevel 1 (
  echo PERSIST AHCI QEMU GATE FAILED
  exit /b 1
)
echo PERSIST AHCI QEMU GATE PASSED
exit /b 0
