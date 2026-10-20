#pragma once
#include "teleop_core.h"
// Stereo world-space wireframes, composited with alpha over passthrough.
class DemoRenderer {
    struct Eye {
        XrSwapchain swap = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageOpenGLESKHR> images;
        int w = 0, h = 0;
        GLuint depth = 0;
    };
    struct Vertex {
        float x, y, z, r, g, b;
    };
    Eye eyes[2];
    GLuint program = 0, fbo = 0, vao = 0, vbo = 0;
    GLint matrixLoc = -1;
    XrCompositionLayerProjectionView projectionViews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                           {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection composition{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<Vertex> vertices;
    static teleop::M orientation(XrQuaternionf q) {
        double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (n < 1e-8)
            return {};
        double x = q.x / n, y = q.y / n, z = q.z / n, w = q.w / n;
        return teleop::M{{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                          2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                          2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    }
    static GLuint shader(GLenum type, const char *text) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &text, nullptr);
        glCompileShader(s);
        GLint ok;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char msg[1024];
            glGetShaderInfoLog(s, sizeof(msg), nullptr, msg);
            LOGE("Demo shader: %s", msg);
            glDeleteShader(s);
            return 0;
        }
        return s;
    }
    void line(teleop::V a, teleop::V b, teleop::V color) {
        auto c = teleop::transpose(teleop::questToBase());
        a = c * a;
        b = c * b;
        vertices.push_back(
            {float(a.x), float(a.y), float(a.z), float(color.x), float(color.y), float(color.z)});
        vertices.push_back(
            {float(b.x), float(b.y), float(b.z), float(color.x), float(color.y), float(color.z)});
    }
    void box(const teleop::Pose &p, teleop::V half, teleop::V color) {
        teleop::V v[8];
        for (int i = 0; i < 8; i++)
            v[i] = p.p + p.r * teleop::V{(i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y,
                                         (i & 4) ? half.z : -half.z};
        for (int i = 0; i < 8; i++)
            for (int k = 0; k < 3; k++)
                if (!(i & (1 << k)))
                    line(v[i], v[i | (1 << k)], color);
    }
    void link(teleop::V a, teleop::V b, double radius, teleop::V color) {
        using namespace teleop;
        V along = unit(b - a);
        V u = unit(cross(along, std::abs(along.z) < .9 ? V{0, 0, 1} : V{0, 1, 0})) * radius,
          v = cross(along, u);
        for (int i = 0; i < 8; i++) {
            double t = i * pi / 4, t1 = (i + 1) * pi / 4;
            V n = u * std::cos(t) + v * std::sin(t), n1 = u * std::cos(t1) + v * std::sin(t1);
            line(a + n, a + n1, color);
            line(b + n, b + n1, color);
            if (i % 2 == 0)
                line(a + n, b + n, color);
        }
    }
    void axes(const teleop::Pose &p, double size) {
        line(p.p, p.p + p.r * teleop::V{size, 0, 0}, {1, .22, .18});
        line(p.p, p.p + p.r * teleop::V{0, size, 0}, {.25, 1, .4});
        line(p.p, p.p + p.r * teleop::V{0, 0, size}, {.3, .5, 1});
    }
    void geometry(const teleop::Robot &r) {
        using namespace teleop;
        vertices.clear();
        V baseColor{.65, .8, .77};
        for (int i = -8; i <= 8; i++) {
            double t = i * .5;
            line(r.ground + V{-4, t, 0}, r.ground + V{4, t, 0}, {.12, .25, .22});
            line(r.ground + V{t, -4, 0}, r.ground + V{t, 4, 0}, {.12, .25, .22});
        }
        box(compose(r.base, {{0, 0, .17}, {}}), {.34, .29, .13}, baseColor);
        box(compose(r.base, {{-.08, 0, .66}, {}}), {.095, .12, .37}, baseColor);
        link(compose(r.base, {{0, -.25, 1.03}, {}}).p, compose(r.base, {{0, .25, 1.03}, {}}).p, .055,
             baseColor);
        for (int x : {-1, 1})
            for (int y : {-1, 1}) {
                Pose wheel = compose(r.base, {{x * .22, y * .3, .12}, rotation({1, 0, 0}, pi / 2)});
                for (int i = 0; i < 16; i++) {
                    double a = i * pi / 8, b = (i + 1) * pi / 8;
                    line(wheel.p + wheel.r * V{.105 * std::cos(a), .105 * std::sin(a), 0},
                         wheel.p + wheel.r * V{.105 * std::cos(b), .105 * std::sin(b), 0}, baseColor);
                }
            }
        V tip = r.base.p + r.base.r * V{.52, 0, .03};
        line(r.base.p + r.base.r * V{.25, 0, .03}, tip, {.3, 1, .5});
        for (int s : {-1, 1})
            line(tip, r.base.p + r.base.r * V{.43, s * .06, .03}, {.3, 1, .5});
        for (int i = 0; i < 2; i++) {
            const auto &a = r.arms[i];
            auto c = a.fk(a.q);
            V color = i == 0 ? V{.15, .85, 1} : V{1, .62, .15};
            for (size_t j = 1; j < c.links.size(); j++)
                link(r.base.p + r.base.r * c.links[j - 1], r.base.p + r.base.r * c.links[j], .027, color);
            for (int j = 0; j < 7; j++) {
                Pose p{r.base.p + r.base.r * c.origins[j], r.base.r};
                box(p, {.028, .028, .028}, color);
            }
            Pose ee = compose(r.base, c.ee);
            axes(ee, .10);
            box(ee, {.025, .055, .025}, color);
            double spread = .012 + .045 * (1 - a.gripper);
            for (int s : {-1, 1})
                box(compose(ee, {{.06, s * spread, 0}, {}}), {.04, .008, .018}, color);
            if (a.engaged) {
                axes(a.targetWorld, .065);
                box(a.targetWorld, {.015, .015, .015}, a.limited ? V{1, .15, .15} : V{.6, 1, .5});
            }
        }
    }

  public:
    static teleop::Pose pose(XrPosef p) {
        return {{p.position.x, p.position.y, p.position.z}, orientation(p.orientation)};
    }
    bool init(XrInstance instance, XrSystemId system, XrSession session) {
        uint32_t n = 0;
        if (!XR_CHECK(xrEnumerateViewConfigurationViews(
                instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr)) ||
            n != 2)
            return false;
        XrViewConfigurationView config[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                             {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        if (!XR_CHECK(xrEnumerateViewConfigurationViews(
                instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, config)))
            return false;
        uint32_t count = 0;
        if (!XR_CHECK(xrEnumerateSwapchainFormats(session, 0, &count, nullptr)))
            return false;
        std::vector<int64_t> formats(count);
        if (!XR_CHECK(xrEnumerateSwapchainFormats(session, count, &count, formats.data())))
            return false;
        int64_t format = 0;
        for (auto f : formats)
            if (f == GL_RGBA8 || f == GL_SRGB8_ALPHA8) {
                format = f;
                if (f == GL_RGBA8)
                    break;
            }
        if (!format)
            return false;
        for (int i = 0; i < 2; i++) {
            auto &e = eyes[i];
            double scale = std::min(1., 1280. / config[i].recommendedImageRectWidth);
            e.w = int(config[i].recommendedImageRectWidth * scale);
            e.h = int(config[i].recommendedImageRectHeight * scale);
            XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            info.format = format;
            info.sampleCount = 1;
            info.width = e.w;
            info.height = e.h;
            info.faceCount = info.arraySize = info.mipCount = 1;
            if (!XR_CHECK(xrCreateSwapchain(session, &info, &e.swap)))
                return false;
            if (!XR_CHECK(xrEnumerateSwapchainImages(e.swap, 0, &count, nullptr)))
                return false;
            e.images.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
            if (!XR_CHECK(xrEnumerateSwapchainImages(
                    e.swap, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader *>(e.images.data()))))
                return false;
            glGenRenderbuffers(1, &e.depth);
            glBindRenderbuffer(GL_RENDERBUFFER, e.depth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, e.w, e.h);
        }
        const char *vs = "#version 300 es\nlayout(location=0) in vec3 p;layout(location=1) in vec3 c;uniform "
                         "mat4 vp;out vec3 color;void main(){gl_Position=vp*vec4(p,1.0);color=c;}";
        const char *fs = "#version 300 es\nprecision mediump float;in vec3 color;out vec4 outColor;void "
                         "main(){outColor=vec4(color,1.0);}";
        GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
        if (!v || !f) {
            if (v)
                glDeleteShader(v);
            if (f)
                glDeleteShader(f);
            return false;
        }
        program = glCreateProgram();
        glAttachShader(program, v);
        glAttachShader(program, f);
        glLinkProgram(program);
        glDeleteShader(v);
        glDeleteShader(f);
        GLint ok;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok)
            return false;
        matrixLoc = glGetUniformLocation(program, "vp");
        glGenFramebuffers(1, &fbo);
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<void *>(3 * sizeof(float)));
        glBindVertexArray(0);
        return glGetError() == GL_NO_ERROR;
    }
    void shutdown() {
        for (auto &e : eyes) {
            if (e.depth)
                glDeleteRenderbuffers(1, &e.depth);
            if (e.swap)
                xrDestroySwapchain(e.swap);
            e.depth = 0;
            e.swap = XR_NULL_HANDLE;
        }
        if (program)
            glDeleteProgram(program);
        if (fbo)
            glDeleteFramebuffers(1, &fbo);
        if (vao)
            glDeleteVertexArrays(1, &vao);
        if (vbo)
            glDeleteBuffers(1, &vbo);
        program = fbo = vao = vbo = 0;
    }
    const XrCompositionLayerBaseHeader *draw(XrSession session, XrSpace space, XrTime time,
                                             const teleop::Robot &robot) {
        if (!program || !robot.placed)
            return nullptr;
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = time;
        locate.space = space;
        XrViewState state{XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t count = 0;
        if (!XR_CHECK(xrLocateViews(session, &locate, &state, 2, &count, views)) || count != 2 ||
            (state.viewStateFlags &
             (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT)) !=
                (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT))
            return nullptr;
        geometry(robot);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glUseProgram(program);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        bool complete = true;
        for (int i = 0; i < 2; i++) {
            auto &e = eyes[i];
            uint32_t index = 0;
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (!XR_CHECK(xrAcquireSwapchainImage(e.swap, &acquire, &index))) {
                complete = false;
                break;
            }
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            if (!XR_CHECK(xrWaitSwapchainImage(e.swap, &wait))) {
                complete = false;
                break;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e.images[index].image,
                                   0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, e.depth);
            glViewport(0, 0, e.w, e.h);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                complete = false;
            auto view = teleop::inverse(pose(views[i].pose));
            // Explicitly clip line segments at the eye near plane. This also avoids
            // driver-dependent wide raster artifacts for ground lines crossing w=0.
            std::vector<Vertex> clipped;
            clipped.reserve(vertices.size());
            for (size_t k = 0; k + 1 < vertices.size(); k += 2) {
                Vertex a = vertices[k], b = vertices[k + 1];
                double za = (view.p + view.r * teleop::V{a.x, a.y, a.z}).z;
                double zb = (view.p + view.r * teleop::V{b.x, b.y, b.z}).z;
                constexpr double plane = -.05;
                if (za > plane && zb > plane)
                    continue;
                if ((za > plane) != (zb > plane)) {
                    double t = (plane - za) / (zb - za);
                    Vertex edge = a;
                    edge.x = a.x + (b.x - a.x) * t;
                    edge.y = a.y + (b.y - a.y) * t;
                    edge.z = a.z + (b.z - a.z) * t;
                    if (za > plane)
                        a = edge;
                    else
                        b = edge;
                }
                clipped.push_back(a);
                clipped.push_back(b);
            }

            float vm[16] = {};
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++)
                    vm[c * 4 + r] = view.r(r, c);
                vm[12 + r] = view.p[r];
            }
            vm[15] = 1;
            const auto &f = views[i].fov;
            float l = std::tan(f.angleLeft), rr = std::tan(f.angleRight), b = std::tan(f.angleDown),
                  t = std::tan(f.angleUp), near = .04f, far = 40;
            float pr[16] = {};
            pr[0] = 2 / (rr - l);
            pr[5] = 2 / (t - b);
            pr[8] = (rr + l) / (rr - l);
            pr[9] = (t + b) / (t - b);
            pr[10] = -(far + near) / (far - near);
            pr[11] = -1;
            pr[14] = -2 * far * near / (far - near);
            float vp[16] = {};
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    for (int k = 0; k < 4; k++)
                        vp[c * 4 + r] += pr[k * 4 + r] * vm[c * 4 + k];
            // Bound raster coordinates before handing long grid lines to GLES.
            std::vector<Vertex> bounded;
            bounded.reserve(clipped.size());
            for (size_t k = 0; k + 1 < clipped.size(); k += 2) {
                Vertex a = clipped[k], b = clipped[k + 1];
                double ca[4] = {}, cb[4] = {};
                for (int r = 0; r < 4; r++) {
                    ca[r] = vp[r] * a.x + vp[4 + r] * a.y + vp[8 + r] * a.z + vp[12 + r];
                    cb[r] = vp[r] * b.x + vp[4 + r] * b.y + vp[8 + r] * b.z + vp[12 + r];
                }
                double lo = 0, hi = 1;
                bool visible = true;
                for (int axis = 0; axis < 3; axis++)
                    for (int sign : {-1, 1}) {
                        double da = ca[3] + sign * ca[axis], db = cb[3] + sign * cb[axis];
                        if (da < 0 && db < 0)
                            visible = false;
                        else if ((da < 0) != (db < 0)) {
                            double t = da / (da - db);
                            if (da < 0)
                                lo = std::max(lo, t);
                            else
                                hi = std::min(hi, t);
                        }
                    }
                if (!visible || lo > hi)
                    continue;
                Vertex aa = a, bb = b;
                aa.x = a.x + (b.x - a.x) * lo;
                aa.y = a.y + (b.y - a.y) * lo;
                aa.z = a.z + (b.z - a.z) * lo;
                bb.x = a.x + (b.x - a.x) * hi;
                bb.y = a.y + (b.y - a.y) * hi;
                bb.z = a.z + (b.z - a.z) * hi;
                bounded.push_back(aa);
                bounded.push_back(bb);
            }
            glBufferData(GL_ARRAY_BUFFER, bounded.size() * sizeof(Vertex), bounded.data(), GL_STREAM_DRAW);
            glUniformMatrix4fv(matrixLoc, 1, GL_FALSE, vp);
            if (complete)
                glDrawArrays(GL_LINES, 0, bounded.size());
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            if (!XR_CHECK(xrReleaseSwapchainImage(e.swap, &release)))
                complete = false;
            auto &p = projectionViews[i];
            p.pose = views[i].pose;
            p.fov = views[i].fov;
            p.subImage.swapchain = e.swap;
            p.subImage.imageRect = {{0, 0}, {e.w, e.h}};
            p.subImage.imageArrayIndex = 0;
        }
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_DEPTH_TEST);
        glUseProgram(0);
        if (!complete)
            return nullptr;
        composition.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        composition.space = space;
        composition.viewCount = 2;
        composition.views = projectionViews;
        return reinterpret_cast<const XrCompositionLayerBaseHeader *>(&composition);
    }
};
