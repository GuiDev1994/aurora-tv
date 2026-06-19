# Build Aurora for LG webOS - PowerShell launcher for WSL
# Run this script on Windows to compile via WSL

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

Write-Host "=== Aurora - Build for LG webOS ===" -ForegroundColor Cyan
Write-Host ""

# Verificar WSL
$wslTest = wsl -e bash -c "echo ok" 2>$null
if ($LASTEXITCODE -ne 0 -or $wslTest -ne "ok") {
    Write-Host "ERRO: WSL nao encontrado ou nao configurado." -ForegroundColor Red
    Write-Host ""
    Write-Host "To install WSL with Ubuntu:" -ForegroundColor Yellow
    Write-Host "  wsl --install -d Ubuntu" -ForegroundColor White
    Write-Host ""
    Write-Host "After installing, restart your computer and run this script again." -ForegroundColor Yellow
    exit 1
}

# Convert Windows path to WSL
$WslPath = wsl -e wslpath -a $ProjectRoot

Write-Host "Projeto: $ProjectRoot" -ForegroundColor Gray
Write-Host "Caminho WSL: $WslPath" -ForegroundColor Gray
Write-Host ""

Write-Host "Iniciando build no WSL (Ubuntu)..." -ForegroundColor Green
Write-Host ""

wsl -e bash -c @"
set -e
cd '$WslPath'
chmod +x scripts/webos/build_for_lg.sh
./scripts/webos/build_for_lg.sh
"@

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "=== Build completed successfully! ===" -ForegroundColor Green
    Write-Host "Pacote IPK em: $ProjectRoot\dist\" -ForegroundColor Cyan
} else {
    Write-Host ""
    Write-Host "Build failed. Check the errors above." -ForegroundColor Red
    exit 1
}
