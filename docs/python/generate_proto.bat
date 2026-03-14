@echo off
:: Generates Python gRPC stubs from camera_service.proto.
:: Run this once before using camera_client.py.
::
:: Usage:  generate_proto.bat [path\to\camera_service.proto]
::
:: If no argument is given the script looks for the proto in the standard
:: location relative to this file: ..\..\proto\camera_service.proto

setlocal

if "%~1"=="" (
    set PROTO_FILE=..\..\proto\camera_service.proto
) else (
    set PROTO_FILE=%~1
)

if not exist "%PROTO_FILE%" (
    echo ERROR: Proto file not found: %PROTO_FILE%
    echo Usage: generate_proto.bat [path\to\camera_service.proto]
    exit /b 1
)

:: Derive the directory containing the proto file (used as -I include path)
for %%F in ("%PROTO_FILE%") do set PROTO_DIR=%%~dpF

:: Output stubs into the same directory as this script
set OUT_DIR=%~dp0

echo Generating Python stubs from %PROTO_FILE%...

python -m grpc_tools.protoc ^
    -I "%PROTO_DIR%" ^
    --python_out="%OUT_DIR%" ^
    --grpc_python_out="%OUT_DIR%" ^
    "%PROTO_FILE%"

if errorlevel 1 (
    echo.
    echo FAILED. Make sure grpcio-tools is installed:
    echo   pip install grpcio-tools
    exit /b 1
)

echo.
echo Generated:
echo   %OUT_DIR%camera_service_pb2.py
echo   %OUT_DIR%camera_service_pb2_grpc.py
echo.
echo You can now run:  python camera_client.py

endlocal
