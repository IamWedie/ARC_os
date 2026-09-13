@echo off
C:\msys64\ucrt64\bin\gcc.exe -ffreestanding -nostdlib -m64 -mno-red-zone -c C:\Users\wadia\os_project\boot\os.S -o C:\Users\wadia\os_project\boot\os.o
C:\msys64\ucrt64\bin\objcopy.exe -O binary C:\Users\wadia\os_project\boot\os.o C:\Users\wadia\os_project\os.bin
C:\msys64\ucrt64\bin\gcc.exe -nostdlib -ffreestanding -m64 -Wl,--nmagic -o C:\Users\wadia\os_project\os.elf C:\Users\wadia\os_project\boot\os.o
echo Build done!
