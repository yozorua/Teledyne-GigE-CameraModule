# "Continue" so cmake warnings on stderr don't abort the pipeline.
# Exit codes are checked explicitly after each cmake call.
$ErrorActionPreference = "Continue"
$LOG = "C:\Users\yozorua\Desktop\Teledyne-GigE-CameraModule\build_camera.log"

# Source vcvars64.bat into the current PowerShell session
$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
Write-Output "=== Sourcing MSVC x64 environment ===" | Tee-Object -FilePath $LOG
cmd /c "`"$vcvars`" && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
    }
}
Write-Output "vcvars done" | Tee-Object -FilePath $LOG -Append

$PROJ   = "C:\Users\yozorua\Desktop\Teledyne-GigE-CameraModule"
$VCPKG  = "C:\Users\yozorua\vcpkg\scripts\buildsystems\vcpkg.cmake"
$BUILD  = "$PROJ\build"
$DIST   = "$PROJ\dist"

Write-Output "=== Configuring ===" | Tee-Object -FilePath $LOG -Append
$configArgs = @(
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_TOOLCHAIN_FILE=$VCPKG",
    "-DVCPKG_TARGET_TRIPLET=x64-windows",
    # VCPKG_APPLOCAL_DEPS is now set in CMakeLists.txt; listed here for clarity.
    "-DVCPKG_APPLOCAL_DEPS=ON",
    "-B", $BUILD,
    $PROJ
)
& cmake @configArgs 2>&1 | Tee-Object -FilePath $LOG -Append
if ($LASTEXITCODE -ne 0) { Write-Output "CONFIGURE FAILED"; exit $LASTEXITCODE }

Write-Output "=== Building ===" | Tee-Object -FilePath $LOG -Append
& cmake --build $BUILD --parallel 2>&1 | Tee-Object -FilePath $LOG -Append
if ($LASTEXITCODE -ne 0) { Write-Output "BUILD FAILED"; exit $LASTEXITCODE }

Write-Output "=== Installing to dist\ ===" | Tee-Object -FilePath $LOG -Append
& cmake --install $BUILD --prefix $DIST 2>&1 | Tee-Object -FilePath $LOG -Append
if ($LASTEXITCODE -ne 0) { Write-Output "INSTALL FAILED"; exit $LASTEXITCODE }

Write-Output "=== BUILD SUCCEEDED ===" | Tee-Object -FilePath $LOG -Append
Write-Output "Deployable files in: $DIST" | Tee-Object -FilePath $LOG -Append
