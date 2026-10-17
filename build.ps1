$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$w64 = "C:\Users\josec\.gemini\antigravity\scratch\w64devkit\bin"
if (Test-Path $w64) {
    $env:PATH = "$w64;$env:PATH"
}
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -S $root -B "$root/build"
cmake --build "$root/build" --parallel 8
Copy-Item "$root/build/SuperMarioBros3.exe" "$root/SuperMarioBros3.exe" -Force
Copy-Item "$root/build/SDL2.dll" "$root/SDL2.dll" -Force
Write-Host "Build completado: $root\SuperMarioBros3.exe" -ForegroundColor Green
