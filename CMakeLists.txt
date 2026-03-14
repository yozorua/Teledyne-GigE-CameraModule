cmake_minimum_required(VERSION 3.21)

# Must be set BEFORE project() so the vcpkg toolchain picks it up.
# Causes vcpkg to add a post-build step that copies all required runtime DLLs
# into the same directory as each executable automatically.
set(VCPKG_APPLOCAL_DEPS ON CACHE BOOL "Copy vcpkg runtime DLLs to build output" FORCE)
set(X_VCPKG_APPLOCAL_DEPS_INSTALL ON CACHE BOOL "Include vcpkg DLLs in cmake --install" FORCE)

project(GigECameraModule CXX)

# ─────────────────────────────────────────────────────────────────────────────
# Language standard
# ─────────────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# ─────────────────────────────────────────────────────────────────────────────
# Strict: reject Visual Studio generators
# Configure with:  cmake -G "Ninja" -DCMAKE_TOOLCHAIN_FILE=... -B build .
# ─────────────────────────────────────────────────────────────────────────────
if(CMAKE_GENERATOR MATCHES "Visual Studio")
    message(FATAL_ERROR
        "Visual Studio generators are not permitted for this project.\n"
        "Use Ninja instead:\n"
        "  cmake -G \"Ninja\" "
        "-DCMAKE_TOOLCHAIN_FILE=<vcpkg_root>/scripts/buildsystems/vcpkg.cmake "
        "-DVCPKG_TARGET_TRIPLET=x64-windows "
        "-DCMAKE_BUILD_TYPE=Release -B build .")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# vcpkg-managed dependencies
# (vcpkg toolchain must be supplied at the cmake command line)
# ─────────────────────────────────────────────────────────────────────────────
find_package(gRPC       CONFIG REQUIRED)
find_package(protobuf   CONFIG REQUIRED)

# ─────────────────────────────────────────────────────────────────────────────
# Spinnaker SDK
# Default path matches the standard Windows installer location.
# Override with:  -DSPINNAKER_DIR="C:/custom/path"
# ─────────────────────────────────────────────────────────────────────────────
set(SPINNAKER_DIR "C:/Program Files/Teledyne/Spinnaker"
    CACHE PATH "Spinnaker SDK root directory")

find_path(SPINNAKER_INCLUDE_DIR
    NAMES Spinnaker.h
    PATHS "${SPINNAKER_DIR}/include"
    REQUIRED)

# The Spinnaker SDK ships a single import library.  Search common sub-dirs
# used across SDK versions.
find_library(SPINNAKER_LIBRARY
    NAMES Spinnaker_v140 Spinnaker_v141 Spinnaker
    PATHS
        "${SPINNAKER_DIR}/lib64/vs2015"
        "${SPINNAKER_DIR}/lib64/vs2022"
        "${SPINNAKER_DIR}/lib"
    REQUIRED)

message(STATUS "Spinnaker include : ${SPINNAKER_INCLUDE_DIR}")
message(STATUS "Spinnaker library : ${SPINNAKER_LIBRARY}")

# ─────────────────────────────────────────────────────────────────────────────
# Protobuf / gRPC code generation
# ─────────────────────────────────────────────────────────────────────────────
set(PROTO_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(PROTO_FILE "${PROTO_DIR}/camera_service.proto")
set(PROTO_OUT  "${CMAKE_CURRENT_BINARY_DIR}/generated")

file(MAKE_DIRECTORY "${PROTO_OUT}")

# Use $<TARGET_FILE:...> generator expressions so CMake resolves the correct
# binary path at build time regardless of build configuration.
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
            "-I${PROTO_DIR}"
            "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}" protobuf::protoc gRPC::grpc_cpp_plugin
    COMMENT "Generating protobuf / gRPC sources from camera_service.proto")

add_custom_target(proto_gen
    DEPENDS
        "${PROTO_OUT}/camera_service.pb.cc"
        "${PROTO_OUT}/camera_service.grpc.pb.cc")

# ─────────────────────────────────────────────────────────────────────────────
# Executable
# ─────────────────────────────────────────────────────────────────────────────
add_executable(GigECameraModule
    src/main.cpp
    src/SharedMemoryManager.cpp
    src/SpinnakerCameraManager.cpp
    src/GrpcServer.cpp
    "${PROTO_OUT}/camera_service.pb.cc"
    "${PROTO_OUT}/camera_service.grpc.pb.cc")

add_dependencies(GigECameraModule proto_gen)

target_include_directories(GigECameraModule PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${SPINNAKER_INCLUDE_DIR}"
    "${PROTO_OUT}")

target_link_libraries(GigECameraModule PRIVATE
    gRPC::grpc++
    gRPC::grpc++_reflection
    protobuf::libprotobuf
    "${SPINNAKER_LIBRARY}"
    Ole32           # COM (required by Windows Imaging Component)
    Windowscodecs)  # WIC — JPEG encoder used by DiskSaveLoop

target_compile_options(GigECameraModule PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:
        /W4       # warning level 4
        /WX-      # warnings are not errors (Spinnaker headers have level-4 warnings)
        /MP       # multi-processor compilation
        /permissive->)

target_compile_definitions(GigECameraModule PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN)

# ─────────────────────────────────────────────────────────────────────────────
# Debug client (interactive gRPC REPL + shared-memory inspector)
# Does NOT link Spinnaker — can run on any machine that has access to the server.
# ─────────────────────────────────────────────────────────────────────────────
add_executable(GigEDebugClient
    src/debug_client.cpp
    "${PROTO_OUT}/camera_service.pb.cc"
    "${PROTO_OUT}/camera_service.grpc.pb.cc")

add_dependencies(GigEDebugClient proto_gen)

target_include_directories(GigEDebugClient PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${PROTO_OUT}")

target_link_libraries(GigEDebugClient PRIVATE
    gRPC::grpc++
    protobuf::libprotobuf)

target_compile_options(GigEDebugClient PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX- /MP /permissive->)

target_compile_definitions(GigEDebugClient PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN)

# ─────────────────────────────────────────────────────────────────────────────
# Install / packaging  (cmake --install build --prefix dist)
# Creates a self-contained directory with executables + all runtime DLLs.
# ─────────────────────────────────────────────────────────────────────────────
install(TARGETS GigECameraModule GigEDebugClient
    RUNTIME DESTINATION .)

# Copy every release DLL from the vcpkg bin directory.
# Excludes debug variants (*d.dll pattern) and the protoc compiler DLL
# which is a build-time tool, not a runtime dependency.
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    install(DIRECTORY "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/"
        DESTINATION .
        FILES_MATCHING PATTERN "*.dll"
        REGEX "/libprotoc\\.dll$" EXCLUDE)   # build-time tool only
endif()
