$ErrorActionPreference = "Stop"

Set-Location $PSScriptRoot

if (Test-Path "build") {
    Write-Host "Removing existing build/..."
    Remove-Item -LiteralPath "build" -Recurse -Force
}

Write-Host "Configuring Windows x64 Release build..."
cmake -B build -S . -A x64

Write-Host "Building VietKi..."
cmake --build build --config Release

Write-Host "Done: build\Release\VietKi.exe"
