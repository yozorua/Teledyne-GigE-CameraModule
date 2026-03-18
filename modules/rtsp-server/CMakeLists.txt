cmake_minimum_required(VERSION 3.21)
project(GigERtspServer CXX)

set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    message(FATAL_ERROR "Use Ninja: cmake -G \"Ninja\" ...")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# gRPC + Protobuf  (vcpkg)
# ─────────────────────────────────────────────────────────────────────────────
find_package(gRPC     CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)

# ─────────────────────────────────────────────────────────────────────────────
# FFmpeg  (vcpkg install "ffmpeg[x264]" --triplet x64-windows)
# avcodec  — H.264 encoding via libx264
# avutil   — core FFmpeg utilities
# swscale  — RGB → YUV420P pixel format conversion
# ─────────────────────────────────────────────────────────────────────────────
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h REQUIRED)

find_library(AVCODEC_LIB NAMES avcodec REQUIRED)
find_library(AVUTIL_LIB  NAMES avutil  REQUIRED)
find_library(SWSCALE_LIB NAMES swscale REQUIRED)

message(STATUS "FFmpeg include : ${FFMPEG_INCLUDE_DIR}")
message(STATUS "avcodec        : ${AVCODEC_LIB}")
message(STATUS "avutil         : ${AVUTIL_LIB}")
message(STATUS "swscale        : ${SWSCALE_LIB}")

# ─────────────────────────────────────────────────────────────────────────────
# Proto / gRPC code generation
# ─────────────────────────────────────────────────────────────────────────────
set(PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../../proto/camera_service.proto")
set(PROTO_OUT  "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${PROTO_OUT}")

add_custom_command(
    OUTPUT
        "${PROTO_OUT}/camera_service.pb.cc"
        "${PROTO_OUT}/camera_service.pb.h"
        "${PROTO_OUT}/camera_service.grpc.pb.cc"
        "${PROTO_OUT}/camera_service.grpc.pb.h"
    COMMAND $<TARGET_FILE:protobuf::protoc>
            "--cpp_out=${PROTO_OUT}"
            "--grpc_out=${PROTO_OUT}"
            "--plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
            "-I${CMAKE_CURRENT_SOURCE_DIR}/../../proto"
            "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}" protobuf::protoc gRPC::grpc_cpp_plugin
    COMMENT "Generating gRPC stubs")

add_custom_target(proto_gen_rtsp DEPENDS
    "${PROTO_OUT}/camera_service.pb.cc"
    "${PROTO_OUT}/camera_service.grpc.pb.cc")

# ─────────────────────────────────────────────────────────────────────────────
# Executable
# ─────────────────────────────────────────────────────────────────────────────
add_executable(GigERtspServer
    src/main.cpp
    src/FrameGrabber.cpp
    src/H264Encoder.cpp
    src/RtspServer.cpp
    "${PROTO_OUT}/camera_service.pb.cc"
    "${PROTO_OUT}/camera_service.grpc.pb.cc")

add_dependencies(GigERtspServer proto_gen_rtsp)

target_include_directories(GigERtspServer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    # gige_camera.h wrapper (header-only, no extra compile unit needed)
    "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/examples/cpp"
    "${FFMPEG_INCLUDE_DIR}"
    "${PROTO_OUT}")

target_link_libraries(GigERtspServer PRIVATE
    gRPC::grpc++
    protobuf::libprotobuf
    ${AVCODEC_LIB}
    ${AVUTIL_LIB}
    ${SWSCALE_LIB}
    Ws2_32)      # Winsock2 for RTSP/RTP TCP+UDP sockets

target_compile_options(GigERtspServer PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX- /MP /permissive->)

target_compile_definitions(GigERtspServer PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN)
