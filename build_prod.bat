@echo off
if not exist .\build mkdir build
FOR %%I IN (.\shaders\*.frag .\shaders\*.vert) DO (
    %VULKAN_SDK%\\Bin\\glslc.exe %%I -o %%I.spv
)
clang.exe -DWIN32 -target x86_64-pc-win32 -c lib\SPIRV-Reflect\spirv_reflect.c -o build/spirv_reflect.o && ^
clang.exe -O2 -ferror-limit=1 -DWIN32 -D_CRT_SECURE_NO_WARNINGS -std=gnu++20 -I .\\lib\\jcwk -I %VULKAN_SDK%\\Include -I .\\lib src/MainWin32.cpp ^
          -l %VULKAN_SDK%\\Lib\\vulkan-1.lib -nostdlib -lmsvcrt -target x86_64-pc-win32 ^
          -luser32.lib -lmincore build/spirv_reflect.o -o build/main.exe && ^
.\\build\\main.exe
