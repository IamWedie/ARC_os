@echo off
setlocal
cd /d "%~dp0.."
echo ============================================
echo   ARC OS AHCI READONLY QEMU GATE
echo ============================================
python tests\boot_test_ahci_readonly.py %*
if errorlevel 1 (
  echo AHCI READONLY QEMU GATE FAILED
  exit /b 1
)
echo AHCI READONLY QEMU GATE PASSED
exit /b 0
