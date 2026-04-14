cmake_minimum_required(VERSION 3.21)
project(GigECameraExamples CXX)

set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# ─────────────────────────────────────────────────────────────────────────────
# Dependencies  (supply vcpkg toolchain at configure time)
# cmake -G "Ninja" -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#       -DVCPKG_TARGET_TRIPLET=x64-windows -DCMAKE_BUILD_TYPE=Release -B build .
# ─────────────────────────────────────────────────────────────────────────────
find_package(gRPC     CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)

# ─────────────────────────────────────────────────────────────────────────────
# Proto / gRPC code generation
# Looks for camera_service.proto in the standard location relative to this file.
# Override with:  -DPROTO_FILE=path/to/camera_service.proto
# ─────────────────────────────────────────────────────────────────────────────
set(PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../../../proto/camera_service.proto"
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
            "-I${CMAKE_CURRENT_SOURCE_DIR}/../../../proto"
            "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}" protobuf::protoc gRPC::grpc_cpp_plugin
    COMMENT "Generating gRPC stubs")

add_custom_target(proto_gen_examples DEPENDS
    "${PROTO_OUT}/camera_service.pb.cc"
    "${PROTO_OUT}/camera_service.grpc.pb.cc")

# ─────────────────────────────────────────────────────────────────────────────
# Helper macro — all examples share the same settings
# ─────────────────────────────────────────────────────────────────────────────
macro(add_gige_example TARGET_NAME SOURCE_FILE)
    add_executable(${TARGET_NAME}
        ${SOURCE_FILE}
        "${PROTO_OUT}/camera_service.pb.cc"
        "${PROTO_OUT}/camera_service.grpc.pb.cc")

    add_dependencies(${TARGET_NAME} proto_gen_examples)

    target_include_directories(${TARGET_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"   # gige_camera.h
        "${PROTO_OUT}")                 # generated proto headers

    target_link_libraries(${TARGET_NAME} PRIVATE
        gRPC::grpc++
        protobuf::libprotobuf)

    target_compile_options(${TARGET_NAME} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX- /MP /permissive->)

    target_compile_definitions(${TARGET_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endmacro()

# ─────────────────────────────────────────────────────────────────────────────
# Examples
# ─────────────────────────────────────────────────────────────────────────────
add_gige_example(simple_grab      simple_grab.cpp)
add_gige_example(camera_control   camera_control.cpp)

# multi_camera_test reads the SHM header directly — needs the project include dir.
add_gige_example(multi_camera_test multi_camera_test.cpp)
target_include_directories(multi_camera_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../include")
