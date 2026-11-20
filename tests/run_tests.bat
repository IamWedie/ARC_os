@echo off
rem ARC OS host-side test runner. Run from anywhere; it cd's to the repo root.
setlocal
cd /d "%~dp0.."

set GCC=C:\msys64\ucrt64\bin\gcc.exe
if not exist "%GCC%" (
    echo ERROR: gcc not found at "%GCC%"
    exit /b 1
)

echo [1/6] Building slab/memory stress test against real kernel sources...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_slab_stress.c tests\pmm_stub.c ^
    kernel\slab.c kernel\memory.c ^
    -o tests\test_slab_stress.exe
if errorlevel 1 exit /b 1

echo [2/6] Running slab/memory stress test...
tests\test_slab_stress.exe
if errorlevel 1 exit /b 1

echo [3/6] Building scheduler test against real kernel sources...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_scheduler.c tests\pmm_stub.c tests\scheduler_stub.c ^
    kernel\scheduler.c ^
    -o tests\test_scheduler.exe
if errorlevel 1 exit /b 1

echo [4/6] Running scheduler test...
tests\test_scheduler.exe
if errorlevel 1 exit /b 1

echo [5/6] Building and running file descriptor test...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_fd.c tests\fd_stub.c ^
    kernel\fd.c ^
    -o tests\test_fd.exe
if errorlevel 1 exit /b 1

tests\test_fd.exe
if errorlevel 1 exit /b 1

echo [6/6] Building and running real filesystem slot test...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_fs_slot.c tests\fs_stub.c tests\pmm_stub.c ^
    kernel\fs.c ^
    -o tests\test_fs_slot.exe
if errorlevel 1 exit /b 1

tests\test_fs_slot.exe
exit /b %errorlevel%
