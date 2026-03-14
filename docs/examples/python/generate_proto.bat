@echo off
:: Generates Python gRPC stubs into this directory.
:: Run once before using gige_camera.py or any example.

setlocal

set PROTO_FILE=..\..\..\proto\camera_service.proto
set OUT_DIR=%~dp0

if not exist "%PROTO_FILE%" (
    echo ERROR: Proto file not found: %PROTO_FILE%
    exit /b 1
)

for %%F in ("%PROTO_FILE%") do set PROTO_DIR=%%~dpF

echo Generating stubs from %PROTO_FILE% ...

python -m grpc_tools.protoc ^
    -I "%PROTO_DIR%" ^
    --python_out="%OUT_DIR%" ^
    --grpc_python_out="%OUT_DIR%" ^
    "%PROTO_FILE%"

if errorlevel 1 (
    echo.
    echo FAILED. Install grpcio-tools first:  pip install grpcio-tools
    exit /b 1
)

echo Done: camera_service_pb2.py and camera_service_pb2_grpc.py generated.
endlocal
