@echo off
setlocal
cd /d "%~dp0.."
echo ============================================
echo   ARC OS AHCI MIXED QEMU GATE
echo ============================================
python tests\boot_test_ahci_mixed.py %*
if errorlevel 1 (
  echo AHCI MIXED QEMU GATE FAILED
  exit /b 1
)
echo AHCI MIXED QEMU GATE PASSED
exit /b 0
