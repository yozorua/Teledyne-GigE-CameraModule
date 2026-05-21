cmake_minimum_required(VERSION 3.21)

# Must be set BEFORE project() so vcpkg copies runtime DLLs automatically.
set(VCPKG_APPLOCAL_DEPS ON CACHE BOOL "" FORCE)
set(X_VCPKG_APPLOCAL_DEPS_INSTALL ON CACHE BOOL "" FORCE)

project(GigELiveView CXX)

set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    message(FATAL_ERROR "Use Ninja: cmake -G \"Ninja\" ...")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# vcpkg dependencies:
#   vcpkg install "imgui[glfw-binding,opengl3-binding]" glfw3 glew grpc protobuf
#         --triplet x64-windows
# ─────────────────────────────────────────────────────────────────────────────
find_package(gRPC     CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(imgui    CONFIG REQUIRED)
find_package(glfw3    CONFIG REQUIRED)
find_package(GLEW     REQUIRED)
find_package(OpenGL   REQUIRED)

# ─────────────────────────────────────────────────────────────────────────────
# Proto code generation (shared .proto from root)
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
    COMMENT "Generating gRPC stubs for GigELiveView")

add_custom_target(proto_gen_liveview DEPENDS
    "${PROTO_OUT}/camera_service.pb.cc"
    "${PROTO_OUT}/camera_service.grpc.pb.cc")

# ─────────────────────────────────────────────────────────────────────────────
# Executable
# ─────────────────────────────────────────────────────────────────────────────
add_executable(GigELiveView
    src/main.cpp
    src/CameraFeed.cpp
    src/LiveViewApp.cpp
    "${PROTO_OUT}/camera_service.pb.cc"
    "${PROTO_OUT}/camera_service.grpc.pb.cc")

add_dependencies(GigELiveView proto_gen_liveview)

target_include_directories(GigELiveView PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/examples/cpp"   # gige_camera.h
    "${PROTO_OUT}")

target_link_libraries(GigELiveView PRIVATE
    gRPC::grpc++
    protobuf::libprotobuf
    imgui::imgui
    glfw
    GLEW::GLEW
    OpenGL::GL)

target_compile_options(GigELiveView PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX- /MP /permissive->)

target_compile_definitions(GigELiveView PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN)

# GUI subsystem — no console window pops up on launch
if(MSVC)
    set_target_properties(GigELiveView PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Install / packaging
# ─────────────────────────────────────────────────────────────────────────────
install(TARGETS GigELiveView RUNTIME DESTINATION .)

if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    install(DIRECTORY "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/"
        DESTINATION .
        FILES_MATCHING PATTERN "*.dll"
        REGEX "/libprotoc\\.dll$" EXCLUDE)
endif()
