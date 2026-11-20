@echo off
rem ARC OS host-side test runner. Run from anywhere; it cd's to the repo root.
setlocal
cd /d "%~dp0.."

set GCC=C:\msys64\ucrt64\bin\gcc.exe
if not exist "%GCC%" (
    echo ERROR: gcc not found at "%GCC%"
    exit /b 1
)

echo [1/10] Building slab/memory stress test against real kernel sources...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_slab_stress.c tests\pmm_stub.c ^
    kernel\slab.c kernel\memory.c ^
    -o tests\test_slab_stress.exe
if errorlevel 1 exit /b 1

echo [2/10] Running slab/memory stress test...
tests\test_slab_stress.exe
if errorlevel 1 exit /b 1

echo [3/10] Building scheduler test against real kernel sources...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_scheduler.c tests\pmm_stub.c tests\scheduler_stub.c ^
    kernel\scheduler.c ^
    -o tests\test_scheduler.exe
if errorlevel 1 exit /b 1

echo [4/10] Running scheduler test...
tests\test_scheduler.exe
if errorlevel 1 exit /b 1

echo [5/10] Building and running file descriptor test...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_fd.c tests\fd_stub.c ^
    kernel\fd.c ^
    -o tests\test_fd.exe
if errorlevel 1 exit /b 1

tests\test_fd.exe
if errorlevel 1 exit /b 1

echo [6/10] Building and running real filesystem slot test...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_fs_slot.c tests\fs_stub.c tests\pmm_stub.c ^
    kernel\fs.c ^
    -o tests\test_fs_slot.exe
if errorlevel 1 exit /b 1

tests\test_fs_slot.exe
if errorlevel 1 exit /b 1

echo [7/10] Building and running page-table core test against real kernel sources...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_pagetable.c tests\pt_stub.c ^
    kernel\pagetable.c ^
    -o tests\test_pagetable.exe
if errorlevel 1 exit /b 1

tests\test_pagetable.exe
if errorlevel 1 exit /b 1

echo [8/10] Building per-process address-space test against real kernel sources...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_address_space.c tests\pmm_stub.c ^
    kernel\address_space.c kernel\pagetable.c ^
    -o tests\test_address_space.exe
if errorlevel 1 exit /b 1

echo [9/10] Running per-process address-space test...
tests\test_address_space.exe
if errorlevel 1 exit /b 1

echo [10/10] Building and running fork/wait test against real kernel sources...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_fork_wait.c tests\fork_wait_stub.c ^
    kernel\scheduler.c ^
    -o tests\test_fork_wait.exe
if errorlevel 1 exit /b 1

tests\test_fork_wait.exe
exit /b %errorlevel%
