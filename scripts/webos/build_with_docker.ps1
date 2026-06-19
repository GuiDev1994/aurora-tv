# Build Aurora for LG webOS via Docker
# Funciona no Windows sem WSL Ubuntu - usa container Ubuntu

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

Write-Host "=== Aurora - Build for LG webOS (via Docker) ===" -ForegroundColor Cyan
Write-Host ""

# Verificar Docker
$dockerOk = $false
$ErrorActionPreferenceBak = $ErrorActionPreference
$ErrorActionPreference = "SilentlyContinue"
& docker ps *>$null
$dockerOk = ($LASTEXITCODE -eq 0)
$ErrorActionPreference = $ErrorActionPreferenceBak
if (-not $dockerOk) {
    Write-Host "ERRO: Docker nao esta rodando ou nao esta instalado." -ForegroundColor Red
    Write-Host "  - Instale Docker Desktop: https://www.docker.com/products/docker-desktop" -ForegroundColor Yellow
    Write-Host "  - Ou use WSL com Ubuntu: wsl --install -d Ubuntu" -ForegroundColor Yellow
    Write-Host "  - Depois execute: ./scripts/webos/build_for_lg.sh (dentro do WSL)" -ForegroundColor Yellow
    exit 1
}

Write-Host "Using Docker to build in Ubuntu environment..." -ForegroundColor Green
Write-Host ""

# Mount the project and run the build
$ScriptPath = Join-Path $PSScriptRoot "docker_build_inner.sh"
# sed removes CRLF (Windows line endings) for Linux compatibility
docker run --rm -v "${ProjectRoot}:/build" -v "${ScriptPath}:/docker_build.sh" -w /build ubuntu:22.04 bash -c "sed 's/\r$//' /docker_build.sh | bash"

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "=== Build concluido! ===" -ForegroundColor Green
    Write-Host "Pacote em: $ProjectRoot\dist\" -ForegroundColor Cyan
    Get-ChildItem "$ProjectRoot\dist\*.ipk" -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $($_.Name)" }
} else {
    Write-Host ""
    Write-Host "Build failed." -ForegroundColor Red
    exit 1
}
