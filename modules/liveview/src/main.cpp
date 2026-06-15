// GigELiveView — Live camera viewer for GigECameraModule.
//
// Connects to the running GigECameraModule via gRPC, reads frames from
// shared memory, and renders a continuously updated live view in a GUI window
// using Dear ImGui + GLFW + OpenGL3.
//
// Usage:
//   GigELiveView.exe
//   (The gRPC server address is entered in the startup dialog.)
//
// Notes:
//   - Does NOT need to run as Administrator — it only reads SHM (OpenFileMapping).
//   - GigECameraModule.exe must already be running and acquiring.

#include <GL/glew.h>        // GLEW must be included before any other GL header
#include <GLFW/glfw3.h>

// Force discrete GPU on hybrid laptops (NVIDIA Optimus / AMD PowerXpress).
// The GPU driver reads these exported symbols before the process starts.
extern "C" { __declspec(dllexport) unsigned long NvOptimusEnablement                = 1; }
extern "C" { __declspec(dllexport) int           AmdPowerXpressRequestHighPerformance = 1; }

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "LiveViewApp.h"

#include <iostream>

static void GlfwErrorCallback(int error, const char* desc) {
    std::cerr << "[GLFW] error " << error << ": " << desc << "\n";
}

int main() {
    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit()) {
        std::cerr << "[main] glfwInit() failed.\n";
        return 1;
    }

    // OpenGL 3.3 Core Profile — required by ImGui's OpenGL3 backend
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);  // start maximized on any resolution

    GLFWwindow* window = glfwCreateWindow(
        1440, 900, "GigE Camera Live View", nullptr, nullptr);
    if (!window) {
        std::cerr << "[main] Failed to create GLFW window.\n";
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // vsync

    // Query OS DPI scale before GLEW init — window must be current.
    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);

    // GLEW must be initialised after the GL context is current.
    glewExperimental = GL_TRUE;
    const GLenum glew_err = glewInit();
    if (glew_err != GLEW_OK) {
        std::cerr << "[main] GLEW init failed: "
                  << glewGetErrorString(glew_err) << "\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // ── Dear ImGui ────────────────────────────────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename  = nullptr;  // don't write imgui.ini

    ImGui::StyleColorsDark();

    // Scale padding/spacing to match the monitor DPI.
    ImGui::GetStyle().ScaleAllSizes(xscale);

    // Load Segoe UI at the native pixel size for the current DPI.
    // Falls back to the built-in bitmap font if the TTF is unavailable.
    const float font_px = 16.0f * xscale;
    ImFont* font = io.Fonts->AddFontFromFileTTF(
        "C:/Windows/Fonts/segoeui.ttf", font_px);
    if (!font)
        io.Fonts->AddFontDefault();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // ── Main loop ─────────────────────────────────────────────────────────────
    LiveViewApp app;

    while (!glfwWindowShouldClose(window) && !app.WantsQuit()) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        app.Render(io.DeltaTime);

        ImGui::Render();

        int fb_w, fb_h;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.08f, 0.08f, 0.08f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
