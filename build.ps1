# build.ps1 — Builds all GigE Camera executables and assembles the dist\ folder.
#
# Outputs:
#   dist\GigECameraModule.exe   — main camera server (requires Spinnaker SDK + Admin)
#   dist\GigEDebugClient.exe    — gRPC REPL + SHM inspector (no Spinnaker needed)
#   dist\GigERTSPStreamer.exe   — RTSP/RTP H.264 streamer (requires FFmpeg + libx264)
#   dist\*.dll                  — all runtime DLLs (vcpkg + MSVC)
#
# Requires: MSVC 2026 Build Tools, CMake + Ninja, vcpkg, Spinnaker SDK, FFmpeg[x264]

$ErrorActionPreference = "Continue"
$LOG = "C:\Users\yozorua\Desktop\Teledyne-GigE-CameraModule\build.log"

# ── Source MSVC x64 environment ───────────────────────────────────────────────
$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
Write-Output "=== Sourcing MSVC x64 environment ===" | Tee-Object -FilePath $LOG
cmd /c "`"$vcvars`" && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
    }
}
Write-Output "vcvars done" | Tee-Object -FilePath $LOG -Append

$PROJ  = "C:\Users\yozorua\Desktop\Teledyne-GigE-CameraModule"
$VCPKG = "C:\Users\yozorua\vcpkg\scripts\buildsystems\vcpkg.cmake"
$BUILD = "$PROJ\build"
$DIST  = "$PROJ\dist"
$RTSP  = "$PROJ\modules\rtsp-streamer"
$RTSPB = "$RTSP\build"

$cmakeCommon = @(
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_TOOLCHAIN_FILE=$VCPKG",
    "-DVCPKG_TARGET_TRIPLET=x64-windows",
    "-DVCPKG_APPLOCAL_DEPS=ON"
)

# ── [1/4] Configure main project ─────────────────────────────────────────────
Write-Output "=== [1/4] Configuring GigECameraModule + GigEDebugClient ===" | Tee-Object -FilePath $LOG -Append
& cmake @cmakeCommon "-B" $BUILD $PROJ 2>&1 | Tee-Object -FilePath $LOG -Append
if ($LASTEXITCODE -ne 0) { Write-Output "CONFIGURE FAILED (main)"; exit $LASTEXITCODE }

# ── [2/4] Build main project ─────────────────────────────────────────────────
Write-Output "=== [2/4] Building ===" | Tee-Object -FilePath $LOG -Append
& cmake --build $BUILD --parallel 2>&1 | Tee-Object -FilePath $LOG -Append
if ($LASTEXITCODE -ne 0) { Write-Output "BUILD FAILED (main)"; exit $LASTEXITCODE }

Write-Output "=== Installing main project to dist\ ===" | Tee-Object -FilePath $LOG -Append
& cmake --install $BUILD --prefix $DIST 2>&1 | Tee-Object -FilePath $LOG -Append
if ($LASTEXITCODE -ne 0) { Write-Output "INSTALL FAILED (main)"; exit $LASTEXITCODE }

# ── [3/4] Configure GigERTSPStreamer ─────────────────────────────────────────
Write-Output "=== [3/4] Configuring GigERTSPStreamer ===" | Tee-Object -FilePath $LOG -Append
& cmake @cmakeCommon "-B" $RTSPB $RTSP 2>&1 | Tee-Object -FilePath $LOG -Append
if ($LASTEXITCODE -ne 0) { Write-Output "CONFIGURE FAILED (GigERTSPStreamer)"; exit $LASTEXITCODE }

# ── [4/4] Build GigERTSPStreamer ─────────────────────────────────────────────
Write-Output "=== [4/4] Building GigERTSPStreamer ===" | Tee-Object -FilePath $LOG -Append
& cmake --build $RTSPB --parallel 2>&1 | Tee-Object -FilePath $LOG -Append
if ($LASTEXITCODE -ne 0) { Write-Output "BUILD FAILED (GigERTSPStreamer)"; exit $LASTEXITCODE }

Write-Output "=== Installing GigERTSPStreamer to dist\ ===" | Tee-Object -FilePath $LOG -Append
& cmake --install $RTSPB --prefix $DIST 2>&1 | Tee-Object -FilePath $LOG -Append
if ($LASTEXITCODE -ne 0) { Write-Output "INSTALL FAILED (GigERTSPStreamer)"; exit $LASTEXITCODE }

# ── Done ─────────────────────────────────────────────────────────────────────
Write-Output "" | Tee-Object -FilePath $LOG -Append
Write-Output "=== BUILD SUCCEEDED ===" | Tee-Object -FilePath $LOG -Append
Write-Output "Deployable files in: $DIST" | Tee-Object -FilePath $LOG -Append
Write-Output "  GigECameraModule.exe  — main camera server" | Tee-Object -FilePath $LOG -Append
Write-Output "  GigEDebugClient.exe   — gRPC REPL + SHM inspector" | Tee-Object -FilePath $LOG -Append
Write-Output "  GigERTSPStreamer.exe  — RTSP/RTP H.264 streamer" | Tee-Object -FilePath $LOG -Append
