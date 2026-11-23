@echo off
setlocal
cd /d "%~dp0.."
echo ============================================
echo   ARC OS AHCI QEMU GATE
echo ============================================
echo [build] AHCI selftest kernel...
set AHCI_SELFTEST=1
python tools\build_kernel.py --quiet
if errorlevel 1 (
  echo AHCI QEMU GATE FAILED: selftest build failed
  set AHCI_SELFTEST=
  python tools\build_kernel.py --quiet >nul 2>nul
  exit /b 1
)
set AHCI_SELFTEST=

python tests\boot_test_ahci.py %*
set RC=%ERRORLEVEL%

echo [build] restoring normal kernel...
python tools\build_kernel.py --quiet
if errorlevel 1 (
  echo AHCI QEMU GATE FAILED: normal rebuild failed
  exit /b 1
)

if not "%RC%"=="0" (
  echo AHCI QEMU GATE FAILED
  exit /b %RC%
)
echo AHCI QEMU GATE PASSED
exit /b 0
