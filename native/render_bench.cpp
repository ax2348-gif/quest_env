// Render benchmark: how much mesh the headset draws in stereo over passthrough before it misses frames, per style
// (mesh_renderer.h: wire, hidden-line, flat) and per draw-call count. Synthetic scene: N moving bodies, each a
// "rock" (a noisy sphere with crease edges, as decimated scene meshes have), in a grid ~2 m ahead of the head.
//
// Sweeps (each step: 1.5 s settle, 4 s measured):
//   triangles  100 bodies x {250 .. 16000} triangles each (25 k .. 1.6 M) x {wire, hidden_line, flat}
//   draw calls 200 k triangles over {20, 100, 500, 2000} bodies, hidden_line
// Per step, one JSON line to logcat (tag QuestRenderBench) and ~/render_bench/<start>.jsonl: frames, display Hz,
// missed frames (display-time gaps > 1.5 periods), CPU ms per frame (draw calls issued, both eyes), GPU ms per
// frame (GL_EXT_disjoint_timer_query when present). Look straight ahead while it runs (~2.5 min); it then exits.
// Launch: sh quest bench.
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "mesh_renderer.h"

#define LOG_TAG "QuestRenderBench"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

int64_t nowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
bool xrOk(XrResult r, const char *what, int line) {
    if (XR_SUCCEEDED(r))
        return true;
    LOGE("OpenXR %s failed at line %d: %d", what, line, r);
    return false;
}
#define XR_CHECK(x) xrOk((x), #x, __LINE__)

// A noisy sphere of about `tris` triangles, radius ~1: crease edges where the noise folds it.
mesh_scene::Mesh rock(int tris, uint32_t seed) {
    int seg = std::max(6, int(std::lround(std::sqrt(tris))));
    int rings = std::max(3, tris / (2 * seg));
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(.85f, 1.15f);
    mesh_scene::Mesh m;
    m.name = "rock";
    std::vector<float> rad((rings + 1) * seg);
    for (auto &r : rad)
        r = u(rng);
    for (int r = 0; r <= rings; r++) {
        double th = M_PI * r / rings;
        for (int i = 0; i < seg; i++) {
            double a = 2 * M_PI * i / seg;
            float k = (r == 0 || r == rings) ? 1.f : rad[r * seg + i];
            m.v.insert(m.v.end(), {float(k * std::sin(th) * std::cos(a)), float(k * std::sin(th) * std::sin(a)),
                                   float(k * std::cos(th))});
        }
    }
    auto at = [&](int r, int i) { return uint32_t(r * seg + (i % seg)); };
    for (int r = 0; r < rings; r++)
        for (int i = 0; i < seg; i++)
            m.tri.insert(m.tri.end(), {at(r, i), at(r + 1, i), at(r + 1, i + 1), at(r, i), at(r + 1, i + 1),
                                       at(r, i + 1)});
    m.edge = mesh_scene::outlineEdges(m.v, m.tri, 30);
    return m;
}

struct Step {
    const char *sweep;
    MeshRenderer::Style style;
    int bodies, trisPerBody;
};

std::vector<Step> plan() {
    std::vector<Step> s;
    for (int t : {250, 500, 1000, 2000, 4000, 8000, 16000})
        for (auto st : {MeshRenderer::Wire, MeshRenderer::HiddenLine, MeshRenderer::Flat})
            s.push_back({"triangles", st, 100, t});
    for (int b : {20, 100, 500, 2000})
        s.push_back({"draw_calls", MeshRenderer::HiddenLine, b, 200000 / b});
    return s;
}

double pct(std::vector<double> v, double p) {
    if (v.empty())
        return NAN;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * (v.size() - 1) + .5))];
}

// GL_EXT_disjoint_timer_query through the core ES 3 query entry points (the extension adds the target).
constexpr GLenum kTimeElapsed = 0x88BF, kGpuDisjoint = 0x8FBB;

class Bench {
  public:
    explicit Bench(android_app *app) : app_(app) {}

    bool init() {
        PFN_xrInitializeLoaderKHR initLoader = nullptr;
        if (XR_FAILED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                            reinterpret_cast<PFN_xrVoidFunction *>(&initLoader))) ||
            !initLoader)
            return false;
        XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        li.applicationVM = app_->activity->vm;
        li.applicationContext = app_->activity->clazz;
        if (!XR_CHECK(initLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&li))))
            return false;
        const char *ext[] = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
                             XR_FB_PASSTHROUGH_EXTENSION_NAME};
        XrInstanceCreateInfoAndroidKHR ai{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        ai.applicationVM = app_->activity->vm;
        ai.applicationActivity = app_->activity->clazz;
        XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
        ci.next = &ai;
        strcpy(ci.applicationInfo.applicationName, "quest_render_bench");
        strcpy(ci.applicationInfo.engineName, "quest_env");
        ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        ci.enabledExtensionCount = 3;
        ci.enabledExtensionNames = ext;
        if (!XR_CHECK(xrCreateInstance(&ci, &instance_)))
            return false;
        XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
        sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        if (!XR_CHECK(xrGetSystem(instance_, &sgi, &system_)))
            return false;
        if (!initEgl())
            return false;
        PFN_xrGetOpenGLESGraphicsRequirementsKHR req = nullptr;
        xrGetInstanceProcAddr(instance_, "xrGetOpenGLESGraphicsRequirementsKHR",
                              reinterpret_cast<PFN_xrVoidFunction *>(&req));
        XrGraphicsRequirementsOpenGLESKHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        if (!req || !XR_CHECK(req(instance_, system_, &gr)))
            return false;
        XrGraphicsBindingOpenGLESAndroidKHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
        gb.display = display_, gb.config = config_, gb.context = context_;
        XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
        sci.next = &gb;
        sci.systemId = system_;
        if (!XR_CHECK(xrCreateSession(instance_, &sci, &session_)))
            return false;
        if (!initPassthrough())
            return false;
        XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rs.poseInReferenceSpace.orientation.w = 1;
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        if (!XR_CHECK(xrCreateReferenceSpace(session_, &rs, &local_)))
            return false;
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        if (!XR_CHECK(xrCreateReferenceSpace(session_, &rs, &view_)))
            return false;
        if (!initSwapchains() || !renderer_.init())
            return false;
        const char *exts = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
        timer_ = exts && std::strstr(exts, "GL_EXT_disjoint_timer_query");
        if (timer_)
            glGenQueries(kQueries, queries_);
        const char *gpu = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
        gpuName_ = gpu ? gpu : "?";
        std::string home = std::string(app_->activity->internalDataPath ? app_->activity->internalDataPath : ".") +
                           "/home/render_bench";
        mkdir(home.c_str(), 0700);
        time_t t = time(nullptr);
        char name[64];
        strftime(name, sizeof name, "/%Y%m%d-%H%M%S.jsonl", localtime(&t));
        out_.open(home + name);
        LOGI("render bench: %s, eye %dx%d, timer queries %s, results %s%s", gpuName_.c_str(), w_, h_,
             timer_ ? "yes" : "no", home.c_str(), name);
        steps_ = plan();
        return true;
    }

    bool running() const { return running_ && !finishing_; }
    void finish() {
        if (finishing_)
            return;
        finishing_ = true;
        if (running_)
            xrRequestExitSession(session_);
        ANativeActivity_finish(app_->activity);
    }

    void pollEvents() {
        XrEventDataBuffer e{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance_, &e) == XR_SUCCESS) {
            if (e.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto s = reinterpret_cast<XrEventDataSessionStateChanged *>(&e)->state;
                if (s == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    running_ = XR_CHECK(xrBeginSession(session_, &bi));
                } else if (s == XR_SESSION_STATE_STOPPING) {
                    running_ = false;
                    XR_CHECK(xrEndSession(session_));
                } else if (s == XR_SESSION_STATE_EXITING || s == XR_SESSION_STATE_LOSS_PENDING) {
                    running_ = false;
                    finish();
                }
            }
            e = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    void frame() {
        XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        if (!XR_CHECK(xrWaitFrame(session_, &wi, &fs)))
            return;
        XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
        XR_CHECK(xrBeginFrame(session_, &bi));
        const int64_t now = nowNs();
        if (!anchored_ && fs.shouldRender)
            anchored_ = anchor(fs.predictedDisplayTime);
        if (anchored_ && stepIndex_ < 0)
            startStep(0, now);
        std::vector<const XrCompositionLayerBaseHeader *> layers;
        XrCompositionLayerPassthroughFB pt{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
        pt.layerHandle = ptLayer_;
        if (fs.shouldRender) {
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&pt));
            if (stepIndex_ >= 0 && stepIndex_ < int(steps_.size())) {
                if (const auto *l = render(fs.predictedDisplayTime, now))
                    layers.push_back(l);
            }
        }
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount = uint32_t(layers.size());
        ei.layers = layers.data();
        XR_CHECK(xrEndFrame(session_, &ei));
        if (stepIndex_ >= 0 && stepIndex_ < int(steps_.size()))
            account(fs, now);
    }

    void shutdown() {
        renderer_.shutdown();
        if (timer_)
            glDeleteQueries(kQueries, queries_);
        for (auto &e : eyes_) {
            if (e.depth)
                glDeleteRenderbuffers(1, &e.depth);
            if (e.swap)
                xrDestroySwapchain(e.swap);
        }
        if (fbo_)
            glDeleteFramebuffers(1, &fbo_);
        if (session_) {
            if (ptLayer_ && destroyLayer_)
                destroyLayer_(ptLayer_);
            if (pt_ && destroyPt_)
                destroyPt_(pt_);
            if (local_)
                xrDestroySpace(local_);
            if (view_)
                xrDestroySpace(view_);
            xrDestroySession(session_);
        }
        if (instance_)
            xrDestroyInstance(instance_);
        if (display_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (surface_ != EGL_NO_SURFACE)
                eglDestroySurface(display_, surface_);
            if (context_ != EGL_NO_CONTEXT)
                eglDestroyContext(display_, context_);
            eglTerminate(display_);
        }
    }

  private:
    struct Eye {
        XrSwapchain swap = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageOpenGLESKHR> images;
        GLuint depth = 0;
    };

    bool initEgl() {
        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr) || !eglBindAPI(EGL_OPENGL_ES_API))
            return false;
        const EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8,
                             EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
        EGLint n = 0;
        if (!eglChooseConfig(display_, ca, &config_, 1, &n) || n < 1)
            return false;
        const EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, xa);
        const EGLint sa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        surface_ = eglCreatePbufferSurface(display_, config_, sa);
        return context_ != EGL_NO_CONTEXT && surface_ != EGL_NO_SURFACE &&
               eglMakeCurrent(display_, surface_, surface_, context_);
    }

    bool initPassthrough() {
        PFN_xrCreatePassthroughFB create = nullptr;
        PFN_xrCreatePassthroughLayerFB createLayer = nullptr;
        xrGetInstanceProcAddr(instance_, "xrCreatePassthroughFB", reinterpret_cast<PFN_xrVoidFunction *>(&create));
        xrGetInstanceProcAddr(instance_, "xrCreatePassthroughLayerFB",
                              reinterpret_cast<PFN_xrVoidFunction *>(&createLayer));
        xrGetInstanceProcAddr(instance_, "xrDestroyPassthroughFB", reinterpret_cast<PFN_xrVoidFunction *>(&destroyPt_));
        xrGetInstanceProcAddr(instance_, "xrDestroyPassthroughLayerFB",
                              reinterpret_cast<PFN_xrVoidFunction *>(&destroyLayer_));
        if (!create || !createLayer)
            return false;
        XrPassthroughCreateInfoFB pc{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
        pc.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
        if (!XR_CHECK(create(session_, &pc, &pt_)))
            return false;
        XrPassthroughLayerCreateInfoFB lc{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
        lc.passthrough = pt_;
        lc.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
        lc.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
        return XR_CHECK(createLayer(session_, &lc, &ptLayer_));
    }

    // The same swapchain sizes as the AR view (demo_renderer.h): the recommended size, capped at 1280 wide.
    bool initSwapchains() {
        uint32_t n = 0;
        XrViewConfigurationView cv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        if (!XR_CHECK(xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                                        &n, cv)) ||
            n != 2)
            return false;
        double s = std::min(1., 1280. / cv[0].recommendedImageRectWidth);
        w_ = int(cv[0].recommendedImageRectWidth * s);
        h_ = int(cv[0].recommendedImageRectHeight * s);
        uint32_t count = 0;
        xrEnumerateSwapchainFormats(session_, 0, &count, nullptr);
        std::vector<int64_t> formats(count);
        xrEnumerateSwapchainFormats(session_, count, &count, formats.data());
        int64_t format = 0;
        for (auto f : formats)
            if (f == GL_RGBA8 || (!format && f == GL_SRGB8_ALPHA8))
                format = f;
        if (!format)
            return false;
        for (auto &e : eyes_) {
            XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            sc.format = format;
            sc.sampleCount = 1;
            sc.width = w_, sc.height = h_;
            sc.faceCount = sc.arraySize = sc.mipCount = 1;
            if (!XR_CHECK(xrCreateSwapchain(session_, &sc, &e.swap)))
                return false;
            xrEnumerateSwapchainImages(e.swap, 0, &count, nullptr);
            e.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
            if (!XR_CHECK(xrEnumerateSwapchainImages(e.swap, count, &count,
                                                     reinterpret_cast<XrSwapchainImageBaseHeader *>(e.images.data()))))
                return false;
            glGenRenderbuffers(1, &e.depth);
            glBindRenderbuffer(GL_RENDERBUFFER, e.depth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w_, h_);
        }
        glGenFramebuffers(1, &fbo_);
        return glGetError() == GL_NO_ERROR;
    }

    // The grid's frame: 2 m ahead of the head at the first tracked pose, level, facing it.
    bool anchor(XrTime t) {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(view_, local_, t, &loc)) ||
            !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
            !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
            return false;
        const auto &q = loc.pose.orientation;
        // the head's forward (-z) in LOCAL, flattened
        float fx = -2 * (q.x * q.z + q.w * q.y), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
        float l = std::sqrt(fx * fx + fz * fz);
        if (l < 1e-3f)
            fx = 0, fz = -1, l = 1;
        fx /= l, fz /= l;
        center_[0] = loc.pose.position.x + 2 * fx;
        center_[1] = loc.pose.position.y;
        center_[2] = loc.pose.position.z + 2 * fz;
        right_[0] = -fz, right_[1] = 0, right_[2] = fx; // forward x up
        return true;
    }

    void startStep(int i, int64_t now) {
        stepIndex_ = i;
        if (i >= int(steps_.size())) {
            LOGI("render bench: done");
            out_.flush();
            finish();
            return;
        }
        const Step &s = steps_[i];
        renderer_.clearMeshes();
        int meshes = std::min(s.bodies, 8); // a few distinct rocks, shared by the bodies
        std::vector<int> ids;
        edgesPerBody_ = 0;
        for (int k = 0; k < meshes; k++) {
            auto m = rock(s.trisPerBody, 7 + k);
            edgesPerBody_ += double(m.nedge()) / meshes;
            trisActual_ = int(m.ntri());
            ids.push_back(renderer_.upload(m));
        }
        int cols = int(std::ceil(std::sqrt(s.bodies * 1.6))), rows = (s.bodies + cols - 1) / cols;
        double pitch = 2.4 / cols, radius = pitch * .38;
        layout_.clear();
        for (int b = 0; b < s.bodies; b++) {
            float rgba[4] = {.35f + .6f * float((b * 37) % 11) / 10, .9f, .6f + .35f * float((b * 17) % 7) / 6, 1};
            renderer_.addInstance(ids[b % meshes], b, mesh_scene::scale(radius, radius, radius), rgba);
            double gx = (b % cols - (cols - 1) / 2.) * pitch, gy = ((rows - 1) / 2. - b / cols) * pitch;
            layout_.push_back({float(gx), float(gy)});
        }
        renderer_.bodies().resize(s.bodies);
        stepStart_ = now;
        frames_ = missed_ = 0;
        lastDisplay_ = 0;
        cpu_.clear(), gpu_.clear();
        period_ = 0;
    }

    const XrCompositionLayerBaseHeader *render(XrTime t, int64_t now) {
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = t;
        li.space = local_;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t n = 0;
        if (XR_FAILED(xrLocateViews(session_, &li, &vs, 2, &n, views)) || n != 2)
            return nullptr;
        // the bodies: on the grid, each turning (poses change every frame, as a pose stream would)
        double tt = (now - stepStart_) * 1e-9;
        auto &bm = renderer_.bodies();
        for (size_t b = 0; b < layout_.size(); b++) {
            double a = tt * (.4 + .05 * (b % 7)) + b;
            float p[7] = {center_[0] + right_[0] * layout_[b][0], center_[1] + layout_[b][1],
                          center_[2] + right_[2] * layout_[b][0], 0, float(std::sin(a / 2)), 0, float(std::cos(a / 2))};
            bm[b] = mesh_scene::fromPose(p);
        }
        renderer_.beginFrame();
        const Step &s = steps_[stepIndex_];
        int64_t c0 = nowNs();
        bool timing = timer_ && frames_ % 2 == 0; // every other frame, read kQueries frames later
        GLuint q = queries_[queryIndex_ % kQueries];
        if (timing) {
            GLint disjoint = 0; // reading it clears a pending disjoint (clock change) before this query
            glGetIntegerv(kGpuDisjoint, &disjoint);
            glBeginQuery(kTimeElapsed, q);
        }
        bool ok = true;
        for (int i = 0; i < 2; i++) {
            auto &e = eyes_[i];
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            if (!XR_CHECK(xrAcquireSwapchainImage(e.swap, &ai, &idx)) || !XR_CHECK(xrWaitSwapchainImage(e.swap, &wi))) {
                ok = false;
                break;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e.images[idx].image, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, e.depth);
            glViewport(0, 0, w_, h_);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            stats_ = renderer_.draw(viewProj(views[i]), s.style);
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            XR_CHECK(xrReleaseSwapchainImage(e.swap, &ri));
            auto &pv = proj_[i];
            pv = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            pv.pose = views[i].pose;
            pv.fov = views[i].fov;
            pv.subImage.swapchain = e.swap;
            pv.subImage.imageRect = {{0, 0}, {w_, h_}};
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (timing) {
            glEndQuery(kTimeElapsed);
            queryIndex_++;
            if (queryIndex_ >= kQueries) { // the oldest query, issued kQueries timed frames ago
                GLuint old = queries_[queryIndex_ % kQueries];
                GLuint avail = 0;
                glGetQueryObjectuiv(old, GL_QUERY_RESULT_AVAILABLE, &avail);
                if (avail) {
                    GLuint ns = 0;
                    glGetQueryObjectuiv(old, GL_QUERY_RESULT, &ns);
                    if (now - stepStart_ > kSettleNs)
                        gpu_.push_back(ns * 1e-6);
                }
            }
        }
        if (now - stepStart_ > kSettleNs)
            cpu_.push_back((nowNs() - c0) * 1e-6);
        if (!ok)
            return nullptr;
        layer_ = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer_.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        layer_.space = local_;
        layer_.viewCount = 2;
        layer_.views = proj_;
        return reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer_);
    }

    static mesh_scene::Mat4 viewProj(const XrView &v) {
        const auto &q = v.pose.orientation;
        float pose[7] = {v.pose.position.x, v.pose.position.y, v.pose.position.z, q.x, q.y, q.z, q.w};
        mesh_scene::Mat4 w = mesh_scene::fromPose(pose), view; // view = inverse(w): R^T, -R^T p
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                view.m[c * 4 + r] = w.m[r * 4 + c];
        for (int r = 0; r < 3; r++)
            view.m[12 + r] = -(view.m[r] * pose[0] + view.m[4 + r] * pose[1] + view.m[8 + r] * pose[2]);
        const float l = std::tan(v.fov.angleLeft), rr = std::tan(v.fov.angleRight), b = std::tan(v.fov.angleDown),
                    t = std::tan(v.fov.angleUp), nr = .04f, fr = 40;
        mesh_scene::Mat4 p;
        std::memset(p.m, 0, sizeof p.m);
        p.m[0] = 2 / (rr - l), p.m[5] = 2 / (t - b), p.m[8] = (rr + l) / (rr - l), p.m[9] = (t + b) / (t - b);
        p.m[10] = -(fr + nr) / (fr - nr), p.m[11] = -1, p.m[14] = -2 * fr * nr / (fr - nr);
        return mesh_scene::mul(p, view);
    }

    void account(const XrFrameState &fs, int64_t now) {
        if (now - stepStart_ <= kSettleNs) {
            lastDisplay_ = fs.predictedDisplayTime;
            return;
        }
        period_ = fs.predictedDisplayPeriod;
        if (lastDisplay_ && fs.predictedDisplayTime - lastDisplay_ > period_ * 3 / 2)
            missed_ += int((fs.predictedDisplayTime - lastDisplay_ + period_ / 2) / period_) - 1;
        lastDisplay_ = fs.predictedDisplayTime;
        frames_++;
        if (now - stepStart_ >= kSettleNs + kMeasureNs) {
            report();
            startStep(stepIndex_ + 1, now);
        }
    }

    void report() {
        const Step &s = steps_[stepIndex_];
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(2);
        double hz = period_ ? 1e9 / period_ : 0;
        o << "{\"sweep\":\"" << s.sweep << "\",\"style\":\"" << MeshRenderer::styleName(s.style)
          << "\",\"bodies\":" << s.bodies << ",\"tris_per_body\":" << trisActual_
          << ",\"triangles\":" << long(trisActual_) * s.bodies << ",\"edges\":" << long(edgesPerBody_ * s.bodies)
          << ",\"draw_calls_per_eye\":" << stats_.drawCalls << ",\"frames\":" << frames_ << ",\"display_hz\":" << hz
          << ",\"missed\":" << missed_ << ",\"missed_pct\":" << 100. * missed_ / std::max(1, frames_ + missed_)
          << ",\"cpu_ms_p50\":" << pct(cpu_, .5) << ",\"cpu_ms_p95\":" << pct(cpu_, .95);
        if (timer_ && !gpu_.empty())
            o << ",\"gpu_ms_p50\":" << pct(gpu_, .5) << ",\"gpu_ms_p95\":" << pct(gpu_, .95);
        o << ",\"eye\":[" << w_ << "," << h_ << "],\"gpu\":\"" << gpuName_ << "\"}";
        LOGI("%s", o.str().c_str());
        out_ << o.str() << "\n";
        out_.flush();
    }

    static constexpr int kQueries = 8;
    static constexpr int64_t kSettleNs = 1500000000LL, kMeasureNs = 4000000000LL;
    android_app *app_;
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace local_ = XR_NULL_HANDLE, view_ = XR_NULL_HANDLE;
    XrPassthroughFB pt_ = XR_NULL_HANDLE;
    XrPassthroughLayerFB ptLayer_ = XR_NULL_HANDLE;
    PFN_xrDestroyPassthroughFB destroyPt_ = nullptr;
    PFN_xrDestroyPassthroughLayerFB destroyLayer_ = nullptr;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    Eye eyes_[2];
    GLuint fbo_ = 0;
    int w_ = 0, h_ = 0;
    XrCompositionLayerProjectionView proj_[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                 {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection layer_{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    MeshRenderer renderer_;
    MeshRenderer::Stats stats_;
    bool running_ = false, finishing_ = false, anchored_ = false, timer_ = false;
    GLuint queries_[kQueries] = {};
    long queryIndex_ = 0;
    float center_[3] = {0, 0, -2}, right_[3] = {1, 0, 0};
    std::vector<std::array<float, 2>> layout_;
    std::vector<Step> steps_;
    int stepIndex_ = -1, frames_ = 0, missed_ = 0, trisActual_ = 0;
    double edgesPerBody_ = 0;
    int64_t stepStart_ = 0;
    XrTime lastDisplay_ = 0;
    XrDuration period_ = 0;
    std::vector<double> cpu_, gpu_;
    std::string gpuName_;
    std::ofstream out_;
};

void handleCmd(android_app *, int32_t) {}

} // namespace

void android_main(android_app *app) {
    app->onAppCmd = handleCmd;
    Bench bench(app);
    const bool ok = bench.init();
    if (!ok) {
        LOGE("render bench failed to initialize");
        bench.finish();
    }
    while (!app->destroyRequested) {
        android_poll_source *source = nullptr;
        int events = 0;
        int timeout = bench.running() ? 0 : 100;
        while (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void **>(&source)) >= 0) {
            timeout = 0;
            if (source)
                source->process(app, source);
            if (app->destroyRequested)
                break;
        }
        if (app->destroyRequested)
            break;
        if (!ok)
            continue;
        bench.pollEvents();
        if (bench.running())
            bench.frame();
    }
    bench.shutdown();
}
