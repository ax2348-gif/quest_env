#include <EGL/egl.h>
#include <GLES3/gl3.h>
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <vector>
#define XR_CHECK(x) ((x) == XR_SUCCESS)
#define LOGE(...)                                                                                            \
    do {                                                                                                     \
        fprintf(stderr, __VA_ARGS__);                                                                        \
        fprintf(stderr, "\n");                                                                               \
    } while (0)
#include "native/demo_renderer.h"
struct Swap {
    GLuint texture;
    int w, h;
};
std::vector<Swap> swaps;
extern "C" {
XrResult xrEnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t cap,
                                           uint32_t *n, XrViewConfigurationView *v) {
    *n = 2;
    if (cap)
        for (int i = 0; i < 2; i++) {
            v[i].recommendedImageRectWidth = 1024;
            v[i].recommendedImageRectHeight = 1024;
        }
    return XR_SUCCESS;
}
XrResult xrEnumerateSwapchainFormats(XrSession, uint32_t cap, uint32_t *n, int64_t *f) {
    *n = 1;
    if (cap)
        *f = GL_RGBA8;
    return XR_SUCCESS;
}
XrResult xrCreateSwapchain(XrSession, const XrSwapchainCreateInfo *i, XrSwapchain *s) {
    Swap v;
    v.w = i->width;
    v.h = i->height;
    glGenTextures(1, &v.texture);
    glBindTexture(GL_TEXTURE_2D, v.texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, v.w, v.h);
    swaps.push_back(v);
    *s = reinterpret_cast<XrSwapchain>(swaps.size());
    return XR_SUCCESS;
}
XrResult xrEnumerateSwapchainImages(XrSwapchain s, uint32_t cap, uint32_t *n, XrSwapchainImageBaseHeader *p) {
    *n = 1;
    if (cap)
        reinterpret_cast<XrSwapchainImageOpenGLESKHR *>(p)->image =
            swaps[reinterpret_cast<size_t>(s) - 1].texture;
    return XR_SUCCESS;
}
XrResult xrAcquireSwapchainImage(XrSwapchain, const XrSwapchainImageAcquireInfo *, uint32_t *i) {
    *i = 0;
    return XR_SUCCESS;
}
XrResult xrWaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo *) { return XR_SUCCESS; }
XrResult xrReleaseSwapchainImage(XrSwapchain s, const XrSwapchainImageReleaseInfo *) {
    auto v = swaps[reinterpret_cast<size_t>(s) - 1];
    std::vector<unsigned char> pixels(v.w * v.h * 4);
    glReadPixels(0, 0, v.w, v.h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    assert(glGetError() == GL_NO_ERROR);
    int colored = 0;
    for (size_t i = 0; i < pixels.size(); i += 4)
        if (pixels[i + 3])
            colored++;
    assert(colored > 1000);
    char file[100];
    snprintf(file, sizeof(file), "/tmp/quest-demo-eye-%zu.ppm", reinterpret_cast<size_t>(s));
    FILE *f = fopen(file, "wb");
    fprintf(f, "P6\n%d %d\n255\n", v.w, v.h);
    for (int y = v.h - 1; y >= 0; y--)
        for (int x = 0; x < v.w; x++) {
            auto *p = &pixels[(y * v.w + x) * 4];
            unsigned char rgb[3] = {p[0], p[1], p[2]};
            if (!p[3]) {
                rgb[0] = 14;
                rgb[1] = 22;
                rgb[2] = 24;
            }
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    printf("Eye %zu: %d colored pixels\n", reinterpret_cast<size_t>(s), colored);
    return XR_SUCCESS;
}
XrResult xrDestroySwapchain(XrSwapchain s) {
    auto v = swaps[reinterpret_cast<size_t>(s) - 1];
    glDeleteTextures(1, &v.texture);
    return XR_SUCCESS;
}
XrResult xrLocateViews(XrSession, const XrViewLocateInfo *, XrViewState *s, uint32_t, uint32_t *n,
                       XrView *v) {
    *n = 2;
    s->viewStateFlags = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    for (int i = 0; i < 2; i++) {
        v[i].pose.position = {i == 0 ? -.032f : .032f, 1.6f, 0};
        v[i].pose.orientation = {-.149438f, 0, 0, .988771f};
        v[i].fov = {-.65f, .65f, .65f, -.65f};
    }
    return XR_SUCCESS;
}
}
int main() {
    EGLDisplay d = eglGetPlatformDisplay(0x31DD, EGL_DEFAULT_DISPLAY, nullptr);
    assert(eglInitialize(d, nullptr, nullptr));
    EGLint attrs[] = {EGL_SURFACE_TYPE,
                      EGL_PBUFFER_BIT,
                      EGL_RENDERABLE_TYPE,
                      0x40,
                      EGL_RED_SIZE,
                      8,
                      EGL_GREEN_SIZE,
                      8,
                      EGL_BLUE_SIZE,
                      8,
                      EGL_NONE};
    EGLConfig cfg;
    EGLint n;
    assert(eglChooseConfig(d, attrs, &cfg, 1, &n) && n);
    EGLint sa[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE}, ca[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    auto surface = eglCreatePbufferSurface(d, cfg, sa);
    auto context = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ca);
    assert(eglMakeCurrent(d, surface, surface, context));
    printf("GL renderer: %s\n", glGetString(GL_RENDERER));
    DemoRenderer renderer;
    assert(renderer.init({}, {}, {}));
    teleop::Robot robot;
    robot.reset({0, 0, 1.6});
    assert(renderer.draw({}, {}, {}, robot));
    renderer.shutdown();
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(d, context);
    eglDestroySurface(d, surface);
    eglTerminate(d);
    puts("PASS real GLES shaders, stereo geometry, projection and alpha rendering (mock XR swapchains)");
}
