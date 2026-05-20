cmake_minimum_required(VERSION 3.21)

# Must be set BEFORE project() so vcpkg copies runtime DLLs automatically.
set(VCPKG_APPLOCAL_DEPS ON CACHE BOOL "" FORCE)
set(X_VCPKG_APPLOCAL_DEPS_INSTALL ON CACHE BOOL "" FORCE)

project(GigERTSPStreamer CXX)

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
# ─────────────────────────────────────────────────────────────────────────────
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h REQUIRED)
find_library(AVCODEC_LIB NAMES avcodec REQUIRED)
find_library(AVUTIL_LIB  NAMES avutil  REQUIRED)
find_library(SWSCALE_LIB NAMES swscale REQUIRED)

message(STATUS "FFmpeg include : ${FFMPEG_INCLUDE_DIR}")
message(STATUS "avcodec        : ${AVCODEC_LIB}")

# ─────────────────────────────────────────────────────────────────────────────
# Proto / gRPC code generation (shared proto from root)
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
    COMMENT "Generating gRPC stubs for GigERTSPStreamer")

add_custom_target(proto_gen_streamer DEPENDS
    "${PROTO_OUT}/camera_service.pb.cc"
    "${PROTO_OUT}/camera_service.grpc.pb.cc")

# ─────────────────────────────────────────────────────────────────────────────
# Executable
# ─────────────────────────────────────────────────────────────────────────────
add_executable(GigERTSPStreamer
    src/main.cpp
    src/FrameGrabber.cpp
    src/H264Encoder.cpp
    src/RtspServer.cpp
    src/RtpPusher.cpp
    "${PROTO_OUT}/camera_service.pb.cc"
    "${PROTO_OUT}/camera_service.grpc.pb.cc")

add_dependencies(GigERTSPStreamer proto_gen_streamer)

target_include_directories(GigERTSPStreamer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/examples/cpp"   # gige_camera.h
    "${FFMPEG_INCLUDE_DIR}"
    "${PROTO_OUT}")

target_link_libraries(GigERTSPStreamer PRIVATE
    gRPC::grpc++
    protobuf::libprotobuf
    ${AVCODEC_LIB}
    ${AVUTIL_LIB}
    ${SWSCALE_LIB}
    Ws2_32)

target_compile_options(GigERTSPStreamer PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX- /MP /permissive->)

target_compile_definitions(GigERTSPStreamer PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN)

# ─────────────────────────────────────────────────────────────────────────────
# Install / packaging  (cmake --install build --prefix dist)
# ─────────────────────────────────────────────────────────────────────────────
install(TARGETS GigERTSPStreamer RUNTIME DESTINATION .)

if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    install(DIRECTORY "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/"
        DESTINATION .
        FILES_MATCHING PATTERN "*.dll"
        REGEX "/libprotoc\\.dll$" EXCLUDE)
endif()
