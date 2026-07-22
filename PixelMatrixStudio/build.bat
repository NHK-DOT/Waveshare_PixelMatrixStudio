@echo off
setlocal
g++ -std=c++17 -O2 tools\make_icon.cpp -o tools\make_icon.exe
if errorlevel 1 exit /b 1
tools\make_icon.exe
if errorlevel 1 exit /b 1
windres PixelMatrixStudio.rc -O coff -o PixelMatrixStudio.res
if errorlevel 1 exit /b 1
g++ -std=c++17 -O2 -finput-charset=UTF-8 -municode -mwindows PixelMatrixStudio.cpp PixelMatrixStudio.res -o PixelMatrixStudio.exe -lgdiplus -lwindowscodecs -lole32 -lcomdlg32 -lcomctl32 -luser32 -lgdi32
if errorlevel 1 exit /b 1
echo Built PixelMatrixStudio.exe
