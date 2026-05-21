# build.ps1 — Builds all GigE Camera executables and assembles the dist\ folder.
#
# Outputs:
#   dist\GigECameraModule.exe   — main camera server (requires Spinnaker SDK + Admin)
#   dist\GigEDebugClient.exe    — gRPC REPL + SHM inspector (no Spinnaker needed)
#   dist\GigERTSPStreamer.exe   — RTSP/RTP H.264 streamer (requires FFmpeg + libx264)
#   dist\GigELiveView.exe       — live camera viewer GUI (requires imgui + glfw3 + glew)
#   dist\*.dll                  — all runtime DLLs (vcpkg + MSVC)
#
# Requires: MSVC 2026 Build Tools, CMake + Ninja, vcpkg, Spinnaker SDK, FFmpeg[x264]
# LiveView extra:
#   vcpkg install "imgui[glfw-binding,opengl3-binding]" glfw3 glew --triplet x64-windows

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

$PROJ     = "C:\Users\yozorua\Desktop\Teledyne-GigE-CameraModule"
$VCPKG    = "C:\Users\yozorua\vcpkg\scripts\buildsystems\vcpkg.cmake"
$BUILD    = "$PROJ\build"
$DIST     = "$PROJ\dist"
$RTSP     = "$PROJ\modules\rtsp-streamer"
$RTSPB    = "$RTSP\build"
$LIVEVIEW = "$PROJ\modules\liveview"
$LIVEVIEWB= "$LIVEVIEW\build"

$cmakeCommon = @(
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_TOOLCHAIN_FILE=$VCPKG",
    "-DVCPKG_TARGET_TRIPLET=x64-windows",
    "-DVCPKG_APPLOCAL_DEPS=ON"
)

# ── [1/6] Configure main project ─────────────────────────────────────────────
Write-Output "=== [1/6] Configuring GigECameraModule + GigEDebugClient ===" | Tee-Object -FilePath $LOG -Append
& cmake @cmakeCommon "-B" $BUILD $PROJ 2>&1 | Tee-Object -FilePath $LOG -Append
if ($LASTEXITCODE -ne 0) { Write-Output "CONFIGURE FAILED (main)"; exit $LASTEXITCODE }

# ── [2/6] Build main project ─────────────────────────────────────────────────
Write-Output "=== [2/6] Building ===" | Tee-Object -FilePath $LOG -Append
& cmake --build $BUILD --parallel 2>&1 | Tee-Object -FilePath $LOG -Append
if ($LASTEXITCODE -ne 0) { Write-Output "BUILD FAILED (main)"; exit $LASTEXITCODE }

Write-Output "=== Installing main project to dist\ ===" | Tee-Object -FilePath $LOG -Append
& cmake --install $BUILD --prefix $DIST 2>&1 | Tee-Object -FilePath $LOG -Append
if ($LASTEXITCODE -ne 0) { Write-Output "INSTALL FAILED (main)"; exit $LASTEXITCODE }

# ── [3/6] Configure GigERTSPStreamer ─────────────────────────────────────────
Write-Output "=== [3/6] Configuring GigERTSPStreamer ===" | Tee-Object -FilePath $LOG -Append
& cmake @cmakeCommon "-B" $RTSPB $RTSP 2>&1 | Tee-Object -FilePath $LOG -Append
if ($LASTEXITCODE -ne 0) { Write-Output "CONFIGURE FAILED (GigERTSPStreamer)"; exit $LASTEXITCODE }

# ── [4/6] Build GigERTSPStreamer ─────────────────────────────────────────────
Write-Output "=== [4/6] Building GigERTSPStreamer ===" | Tee-Object -FilePath $LOG -Append
& cmake --build $RTSPB --parallel 2>&1 | Tee-Object -FilePath $LOG -Append
if ($LASTEXITCODE -ne 0) { Write-Output "BUILD FAILED (GigERTSPStreamer)"; exit $LASTEXITCODE }

Write-Output "=== Installing GigERTSPStreamer to dist\ ===" | Tee-Object -FilePath $LOG -Append
& cmake --install $RTSPB --prefix $DIST 2>&1 | Tee-Object -FilePath $LOG -Append
if ($LASTEXITCODE -ne 0) { Write-Output "INSTALL FAILED (GigERTSPStreamer)"; exit $LASTEXITCODE }

# ── [5/6] Configure GigELiveView ─────────────────────────────────────────────
Write-Output "=== [5/6] Configuring GigELiveView ===" | Tee-Object -FilePath $LOG -Append
& cmake @cmakeCommon "-B" $LIVEVIEWB $LIVEVIEW 2>&1 | Tee-Object -FilePath $LOG -Append
if ($LASTEXITCODE -ne 0) { Write-Output "CONFIGURE FAILED (GigELiveView)"; exit $LASTEXITCODE }

# ── [6/6] Build GigELiveView ─────────────────────────────────────────────────
Write-Output "=== [6/6] Building GigELiveView ===" | Tee-Object -FilePath $LOG -Append
& cmake --build $LIVEVIEWB --parallel 2>&1 | Tee-Object -FilePath $LOG -Append
if ($LASTEXITCODE -ne 0) { Write-Output "BUILD FAILED (GigELiveView)"; exit $LASTEXITCODE }

Write-Output "=== Installing GigELiveView to dist\ ===" | Tee-Object -FilePath $LOG -Append
& cmake --install $LIVEVIEWB --prefix $DIST 2>&1 | Tee-Object -FilePath $LOG -Append
if ($LASTEXITCODE -ne 0) { Write-Output "INSTALL FAILED (GigELiveView)"; exit $LASTEXITCODE }

# ── Clean up build directories (all deployable files are now in dist\) ───────
Write-Output "=== Removing build directories ===" | Tee-Object -FilePath $LOG -Append
Remove-Item -Recurse -Force $BUILD     -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $RTSPB     -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $LIVEVIEWB -ErrorAction SilentlyContinue
Write-Output "Build directories removed." | Tee-Object -FilePath $LOG -Append

# ── Done ─────────────────────────────────────────────────────────────────────
Write-Output "" | Tee-Object -FilePath $LOG -Append
Write-Output "=== BUILD SUCCEEDED ===" | Tee-Object -FilePath $LOG -Append
Write-Output "Deployable files in: $DIST" | Tee-Object -FilePath $LOG -Append
Write-Output "  GigECameraModule.exe  — main camera server" | Tee-Object -FilePath $LOG -Append
Write-Output "  GigEDebugClient.exe   — gRPC REPL + SHM inspector" | Tee-Object -FilePath $LOG -Append
Write-Output "  GigERTSPStreamer.exe  — RTSP/RTP H.264 streamer" | Tee-Object -FilePath $LOG -Append
Write-Output "  GigELiveView.exe      — live camera viewer GUI" | Tee-Object -FilePath $LOG -Append
