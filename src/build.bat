@echo off

call "C:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat" x64

mkdir build
pushd build
cl -Zi ..\src\win32_cgame.cpp user32.lib Gdi32.lib
popd
