@echo off
rem ARC OS host-side test runner. Run from anywhere; it cd's to the repo root.
setlocal
cd /d "%~dp0.."

set GCC=C:\msys64\ucrt64\bin\gcc.exe
if not exist "%GCC%" (
    echo ERROR: gcc not found at "%GCC%"
    exit /b 1
)

echo [1/11] Building slab/memory stress test against real kernel sources...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_slab_stress.c tests\pmm_stub.c ^
    kernel\slab.c kernel\memory.c ^
    -o tests\test_slab_stress.exe
if errorlevel 1 exit /b 1

echo [2/11] Running slab/memory stress test...
tests\test_slab_stress.exe
if errorlevel 1 exit /b 1

echo [3/11] Building scheduler test against real kernel sources...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_scheduler.c tests\pmm_stub.c tests\scheduler_stub.c ^
    kernel\scheduler.c ^
    -o tests\test_scheduler.exe
if errorlevel 1 exit /b 1

echo [4/11] Running scheduler test...
tests\test_scheduler.exe
if errorlevel 1 exit /b 1

echo [5/11] Building and running file descriptor test...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_fd.c tests\fd_stub.c ^
    kernel\fd.c ^
    -o tests\test_fd.exe
if errorlevel 1 exit /b 1

tests\test_fd.exe
if errorlevel 1 exit /b 1

echo [6/11] Building and running real filesystem slot test...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_fs_slot.c tests\fs_stub.c tests\pmm_stub.c ^
    kernel\fs.c ^
    -o tests\test_fs_slot.exe
if errorlevel 1 exit /b 1

tests\test_fs_slot.exe
if errorlevel 1 exit /b 1

echo [7/11] Building and running page-table core test against real kernel sources...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_pagetable.c tests\pt_stub.c ^
    kernel\pagetable.c ^
    -o tests\test_pagetable.exe
if errorlevel 1 exit /b 1

tests\test_pagetable.exe
if errorlevel 1 exit /b 1

echo [8/11] Building per-process address-space test against real kernel sources...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_address_space.c tests\pmm_stub.c ^
    kernel\address_space.c kernel\pagetable.c ^
    -o tests\test_address_space.exe
if errorlevel 1 exit /b 1

echo [9/11] Running per-process address-space test...
tests\test_address_space.exe
if errorlevel 1 exit /b 1

echo [10/11] Building and running fork/wait test against real kernel sources...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_fork_wait.c tests\fork_wait_stub.c ^
    kernel\scheduler.c kernel\fd.c kernel\address_space.c ^
    kernel\pagetable.c kernel\fork.c kernel\syscall.c kernel\exec.c ^
    -o tests\test_fork_wait.exe
if errorlevel 1 exit /b 1

tests\test_fork_wait.exe
if errorlevel 1 exit /b 1

echo [11/11] Building and running exec-loader test against real kernel sources...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_exec.c tests\exec_stub.c ^
    kernel\scheduler.c kernel\address_space.c kernel\pagetable.c ^
    kernel\exec.c kernel\fd.c kernel\syscall.c kernel\fork.c ^
    -o tests\test_exec.exe
if errorlevel 1 exit /b 1

tests\test_exec.exe
if errorlevel 1 exit /b 1

echo [12/12] Building and running gfx test against real kernel sources...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_gfx.c tests\gfx_stub.c ^
    kernel\gfx.c ^
    -o tests\test_gfx.exe
if errorlevel 1 exit /b 1

tests\test_gfx.exe
if errorlevel 1 exit /b 1

echo [13/14] Building and running gui test against real kernel sources...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_gui.c tests\gui_stub.c ^
    kernel\mouse.c kernel\compositor.c kernel\gui.c ^
    -o tests\test_gui.exe
if errorlevel 1 exit /b 1

tests\test_gui.exe
if errorlevel 1 exit /b 1

echo [14/14] Building and running bootinfo/MB2 parser test...
"%GCC%" -O2 -Wall -Wextra -std=gnu11 -I kernel ^
    tests\test_bootinfo.c ^
    kernel\bootinfo.c ^
    -o tests\test_bootinfo.exe
if errorlevel 1 exit /b 1

tests\test_bootinfo.exe
exit /b %errorlevel%
