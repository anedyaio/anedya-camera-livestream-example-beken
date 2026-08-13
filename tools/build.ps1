# Build the camera firmware on Windows.
#
# Everything happens inside Beken's official Docker image, so the toolchain is
# identical to Linux and macOS. Requires Docker Desktop with the WSL2 backend.
#
#     .\tools\build.ps1            build
#     .\tools\build.ps1 clean      remove build output
#
# Runs from any directory.

$ErrorActionPreference = "Stop"

$Image = "bekencorp/armino-idk:1.2"
$Soc   = "bk7258"

$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $RepoRoot

# ── Preflight ────────────────────────────────────────────────────────────────

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Host "Docker is not installed, or is not on your PATH."
    Write-Host "  https://docs.docker.com/desktop/install/windows-install/  (use the WSL2 backend)"
    exit 1
}

docker info *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Cannot reach the Docker daemon. Is Docker Desktop running?"
    exit 1
}

if (-not (Test-Path "sdk/Makefile") -or -not (Test-Path "libpeer/CMakeLists.txt")) {
    Write-Host "The submodules are missing. Fetch them with:"
    Write-Host ""
    Write-Host "    git submodule update --init --recursive"
    exit 1
}

# ── Build ────────────────────────────────────────────────────────────────────
#
# The application and libpeer live in THIS repository rather than inside the
# SDK, so the SDK stays a pristine checkout of Beken's tree. PROJECT_DIR and
# EXTRA_COMPONENTS_DIRS are what let the build find them; make must run from
# the SDK root because ARMINO_DIR is derived from the working directory.

$BuildArgs = $args
if ($BuildArgs.Count -eq 0) {
    $BuildArgs = @("make", $Soc, "PROJECT=anedya-camera-livestream")
}

# Docker on Windows needs a Linux-style path for the bind mount.
$MountPath = $RepoRoot -replace '\\', '/'

Write-Host "Building for $Soc ..."
docker run --rm `
    -v "${MountPath}:/armino" `
    -w /armino/sdk `
    -e PROJECT_DIR=/armino/app `
    -e EXTRA_COMPONENTS_DIRS=/armino/libpeer `
    $Image @BuildArgs

if ($LASTEXITCODE -eq 0) {
    $Firmware = Get-ChildItem -Path "sdk/build" -Filter "all-app.bin" -Recurse -ErrorAction SilentlyContinue |
                Select-Object -First 1
    if ($Firmware) {
        Write-Host ""
        Write-Host "Firmware: $($Firmware.FullName)"
        Write-Host "See the README for how to flash it."
    }
}
