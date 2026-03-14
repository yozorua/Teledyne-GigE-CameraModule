cmake_minimum_required(VERSION 3.21)
project(CameraAnalyzerExample CXX)

set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# ─────────────────────────────────────────────────────────────────────────────
# Dependencies (via vcpkg toolchain — supply at configure time)
# cmake -G "Ninja" -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#       -DVCPKG_TARGET_TRIPLET=x64-windows -DCMAKE_BUILD_TYPE=Release -B build .
# ─────────────────────────────────────────────────────────────────────────────
find_package(gRPC     CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)

# ─────────────────────────────────────────────────────────────────────────────
# Proto / gRPC code generation
#
# Point PROTO_FILE at the .proto shipped with GigECameraModule, or copy it
# alongside this CMakeLists.txt and set PROTO_FILE to the local copy.
# ─────────────────────────────────────────────────────────────────────────────
set(PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/camera_service.proto"
    CACHE PATH "Path to camera_service.proto")

set(PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated")
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
            "-I${CMAKE_CURRENT_SOURCE_DIR}"
            "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}" protobuf::protoc gRPC::grpc_cpp_plugin
    COMMENT "Generating protobuf / gRPC stubs")

add_custom_target(proto_gen_example
    DEPENDS
        "${PROTO_OUT}/camera_service.pb.cc"
        "${PROTO_OUT}/camera_service.grpc.pb.cc")

# ─────────────────────────────────────────────────────────────────────────────
# Executable
# ─────────────────────────────────────────────────────────────────────────────
add_executable(camera_analyzer
    camera_analyzer.cpp
    "${PROTO_OUT}/camera_service.pb.cc"
    "${PROTO_OUT}/camera_service.grpc.pb.cc")

add_dependencies(camera_analyzer proto_gen_example)

target_include_directories(camera_analyzer PRIVATE "${PROTO_OUT}")

target_link_libraries(camera_analyzer PRIVATE
    gRPC::grpc++
    protobuf::libprotobuf)

target_compile_options(camera_analyzer PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX- /MP /permissive->)

target_compile_definitions(camera_analyzer PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN)
