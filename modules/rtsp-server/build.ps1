$ErrorActionPreference = "Continue"

$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
Write-Output "=== Sourcing MSVC x64 environment ==="
cmd /c "`"$vcvars`" && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
    }
}

$PROJ  = "C:\Users\yozorua\Desktop\Teledyne-GigE-CameraModule\modules\rtsp-server"
$VCPKG = "C:\Users\yozorua\vcpkg\scripts\buildsystems\vcpkg.cmake"
$BUILD = "$PROJ\build"

Write-Output "=== Configuring ==="
$configArgs = @(
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_TOOLCHAIN_FILE=$VCPKG",
    "-DVCPKG_TARGET_TRIPLET=x64-windows",
    "-B", $BUILD,
    $PROJ
)
& cmake @configArgs 2>&1
if ($LASTEXITCODE -ne 0) { Write-Output "CONFIGURE FAILED"; exit $LASTEXITCODE }

Write-Output "=== Building ==="
& cmake --build $BUILD --parallel 2>&1
if ($LASTEXITCODE -ne 0) { Write-Output "BUILD FAILED"; exit $LASTEXITCODE }

Write-Output "=== BUILD SUCCEEDED ==="
Write-Output "Binary: $BUILD\GigERtspServer.exe"
