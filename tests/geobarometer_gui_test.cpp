/*
 easyMelts geobarometer (c) 2026 Eric C. P. Breard. Free software under the GNU General Public License v3 (see LICENSE.txt).

 Smoke test of the easyMelts Geobarometer tab: hidden window, the tab brought to the front,
 a run on the Bishop Tuff sample started with the Run button hook, frames drawn until it
 finishes, then the framebuffer written to a PPM file.

   geobarometer_gui_test [out.ppm]
*/

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "glad.h"
#include "GLFW/glfw3.h"

#include "imgui_opengl.hpp"

static void ReportGlfwError(int code, const char *text) { std::fprintf(stderr, "GLFW error %d: %s
", code, text); }

struct GeobarometerGuiTest {
    static int Run(const char *ppm, int width, int height) {
        glfwSetErrorCallback(ReportGlfwError);
        if (!glfwInit()) return 2;
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        GLFWwindow *window = glfwCreateWindow(width, height, "easyMelts geobarometer test", NULL, NULL);
        if (!window) return 3;
        glfwMakeContextCurrent(window);
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return 4;
        glViewport(0, 0, width, height);
        glClearColor(0, 0.5, 0.8, 1);

        int status = 0;
        {
            ImGuiOpenGL gui(window);
            gui._MI.InitializeMelts(MODE__MELTS);
            gui.m_Composition = {74.39, 0.180, 13.55, 0.36, 0.000, 0.976, 0.00, 0.5, 0.0, 0.0, 1.43, 3.36, 5.09, 0.00, 10.0, 0.00, 0.00, 0.00, 0.00, 0.00};
            gui.m_GbSelectTab = true;
            int frames = 0;
            auto frame = [&]() {
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                gui.UpdateAndRender();
                glFinish();
                ++frames;
            };
            for (int i = 0; i < 3; ++i) frame();
            gui.m_GbAutoRun = true;
            frame();
            if (!gui.m_GbFuture.valid()) {
                std::fprintf(stderr, "run did not start: %s\n", gui.m_GbMessage.c_str());
                status = 5;
            }
            auto t0 = std::chrono::steady_clock::now();
            while (gui.m_GbFuture.valid()) {
                frame();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            for (int i = 0; i < 5; ++i) frame();
            double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

            auto save = [&](const std::string &name) {
                std::vector<unsigned char> px((size_t)width * height * 3);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadBuffer(GL_BACK);
                glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, px.data());
                if (FILE *f = std::fopen(name.c_str(), "wb")) {
                    std::fprintf(f, "P6\n%d %d\n255\n", width, height);
                    for (int y = height - 1; y >= 0; --y) std::fwrite(&px[(size_t)y * width * 3], 1, (size_t)width * 3, f);
                    std::fclose(f);
                }
            };
            auto report = [&]() {
                std::printf("frames %d, run %.1f s, runs %zu, message: %s\n", frames, sec, gui.m_GbRuns.size(), gui.m_GbMessage.c_str());
                for (const auto &r : gui.m_GbRuns)
                    std::printf("  %s %+.2f  P3 %s (%.1f C)  P2 %s (%.1f C, %s)\n", r.sample.c_str(), r.fo2_offset,
                                r.fit3.estimated ? std::to_string(r.fit3.p_est).c_str() : "-", r.fit3.min_residual,
                                r.fit2.estimated ? std::to_string(r.fit2.p_est).c_str() : "-", r.fit2.min_residual,
                                r.fit2.phases_at_min.c_str());
            };
            save(ppm);
            report();

            // Second run as in Ruefer et al. (2025): quartz, plagioclase, orthopyroxene at three offsets.
            gui.m_GbSettings.phases = {{"quartz", "feldspar", "orthopyroxene"}};
            std::snprintf(gui.m_GbOffsets, sizeof gui.m_GbOffsets, "-1, 0, 0.5");
            gui.m_GbAutoRun = true;
            frame();
            t0 = std::chrono::steady_clock::now();
            while (gui.m_GbFuture.valid()) {
                frame();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            gui.m_GbSelected = 1;
            for (int i = 0; i < 5; ++i) frame();
            sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::string second(ppm);
            second.insert(second.size() - 4, "_opx");
            save(second);
            report();
            gui.DestroyAssets();
        }
        glfwTerminate();
        return status;
    }
};

int main(int argc, char **argv) {
    return GeobarometerGuiTest::Run(argc > 1 ? argv[1] : "geobarometer_gui_test.ppm", 1600, 900);
}
