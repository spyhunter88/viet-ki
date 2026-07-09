@echo off
setlocal

cd /d "%~dp0"

if exist build (
  echo Removing existing build/...
  rmdir /s /q build
)

echo Configuring Windows x64 Release build...
cmake -B build -S . -A x64
if errorlevel 1 exit /b %errorlevel%

echo Building VietKi...
cmake --build build --config Release
if errorlevel 1 exit /b %errorlevel%

echo Done: build\Release\VietKi.exe
