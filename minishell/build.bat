@echo off
rem Builds the shell (Cygwin gcc) and its terminal window (MSYS2 MinGW gcc + raylib).
rem Then double-click terminal.exe to start.
cd /d "%~dp0"

echo [1/2] minishell.exe
C:\cygwin64\bin\gcc.exe -Wall -Wextra -O2 -o minishell.exe Minishell.c || goto :fail

echo [2/2] terminal.exe
set PATH=C:\msys64\mingw64\bin;%PATH%
gcc -Wall -Wextra -O2 -o terminal.exe terminal\terminal.c terminal\proc_win.c ^
    -static-libgcc -l:libraylib.a -lglfw3 -lopengl32 -lgdi32 -lwinmm -mwindows || goto :fail
rem MSYS2's raylib uses GLFW as a DLL: keep a copy next to terminal.exe
copy /y C:\msys64\mingw64\bin\glfw3.dll . >nul || goto :fail

echo Build OK - run terminal.exe
exit /b 0

:fail
echo Build FAILED
exit /b 1
