@echo off
set CC=C:\msys64\ucrt64\bin\gcc.exe
set OBJCOPY=C:\msys64\ucrt64\bin\objcopy.exe
set BASE=C:\Users\wadia\os_project

echo === ARC OS v1.0 - Full Build System ===
echo.

REM ============================================
REM Step 1: Compile bootloader
REM ============================================
echo [1/3] Compiling bootloader...
%CC% -ffreestanding -nostdlib -m64 -mno-red-zone -c %BASE%\boot\os.S -o %BASE%\boot\os.o

REM ============================================
REM Step 2: Create binaries
REM ============================================
echo [2/3] Creating binaries...
%OBJCOPY% -O binary %BASE%\boot\os.o %BASE%\os.bin

echo [3/3] Creating disk image...
%OBJCOPY% -O binary %BASE%\boot\os.o %BASE%\kernel.bin

echo.
echo ============================================
echo   BUILD COMPLETE
echo ============================================
echo.
echo   os.bin       - Flat binary (%BASE%\os.bin)
echo   kernel.bin   - Standalone binary (%BASE%\kernel.bin)
echo.
echo   Run with QEMU:
echo     qemu-system-x86_64 -kernel os.bin -nographic -m 512M
echo     qemu-system-x86_64 -drive format=raw,file=kernel.bin -nographic -m 512M
echo     qemu-system-x86_64 -cdrom os.iso -nographic -m 512M
echo.
echo   Create ISO:
echo     make_iso.bat
echo.
