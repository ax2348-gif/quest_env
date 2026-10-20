// Transparent, premultiplied-alpha HUD and small controller markers.
class StatusPanel {
public:
    bool init(XrSession session, bool controller = false) {
        controllerMode = controller;
        width = controller ? 192 : 1024;
        height = controller ? 192 : 768;
        uint32_t count = 0;
        if (!XR_CHECK(xrEnumerateSwapchainFormats(session, 0, &count, nullptr))) return false;
        std::vector<int64_t> formats(count);
        if (!XR_CHECK(xrEnumerateSwapchainFormats(session, count, &count, formats.data()))) return false;
        int64_t format = 0;
        for (auto candidate : formats)
            if (candidate == GL_RGBA8 || candidate == GL_SRGB8_ALPHA8) {
                format = candidate;
                if (candidate == GL_RGBA8) break;
            }
        if (!format) return false;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format = format;
        info.sampleCount = 1; info.width = width; info.height = height;
        info.faceCount = 1; info.arraySize = 1; info.mipCount = 1;
        if (!XR_CHECK(xrCreateSwapchain(session, &info, &swapchain))) return false;
        if (!XR_CHECK(xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr))) return false;
        images.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        if (!XR_CHECK(xrEnumerateSwapchainImages(swapchain, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())))) return false;
        glGenFramebuffers(1, &fbo);
        return fbo != 0;
    }

    void shutdown() {
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (swapchain != XR_NULL_HANDLE) xrDestroySwapchain(swapchain);
        fbo = 0; swapchain = XR_NULL_HANDLE; ready = false;
    }

    bool draw(bool focused, bool head, bool left, bool right, bool connected,
              float hold, float hz, float inputMs, float grip = 0,
              XrVector2f stick = {0, 0}, char side = 'L') {
        int64_t now = monotonicNs();
        if (!born) born = now;
        // Raster artwork updates at 10 Hz; layer poses still update every XR frame.
        if (ready && now - lastDraw < 100000000LL) return true;
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t index = 0;
        if (!XR_CHECK(xrAcquireSwapchainImage(swapchain, &acquire, &index))) return false;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        if (!XR_CHECK(xrWaitSwapchainImage(swapchain, &wait))) return false;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               images[index].image, 0);
        bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (complete) {
            glDisable(GL_SCISSOR_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_SCISSOR_TEST);
            if (controllerMode) {
                green(0.16f);
                circle(96, 96, 65);
                ticks(96, 96, 65, 12);
                green(0.32f);
                text(91, 21, std::string(1, side), 2);
                // A thumbstick dot and a squeeze arc; no controller mesh or rays.
                rect(94 + static_cast<int>(stick.x * 40),
                     94 - static_cast<int>(stick.y * 40), 4, 4);
                green(0.35f);
                arc(96, 96, 72, fmaxf(0, fminf(1, grip)));
            } else {
                const float elapsed = static_cast<float>(now - born) / 1e9f;
                const float fade = fmaxf(0, fminf(1, (6.0f - elapsed) / 2.0f));
                if (fade > 0) {
                    green(0.5f * fade);
                    text(377, 264, "QUEST TELEMETRY", 3);
                    text(368, 300, "HOLD B 2S TO STOP", 3);
                }
                green(focused ? 0.13f : 0.06f);
                circle(512, 384, 28);
                ticks(512, 384, 28, 12);
                // Keep the centre open, with four tiny index marks.
                rect(510, 343, 4, 5); rect(510, 420, 4, 5);
                rect(471, 382, 5, 4); rect(548, 382, 5, 4);
                green(0.32f);
                text(28, 674, "XR " + fixed(hz, 0) + " HZ", 2);
                text(28, 702, "INPUT " + fixed(inputMs, 2) + " MS", 2);
                text(696, 674, std::string("H ") + (head ? "OK" : "--") +
                     "  L " + (left ? "OK" : "--") + "  R " + (right ? "OK" : "--"), 2);
                text(696, 702, connected ? "STREAM CONNECTED" : "STREAM WAITING", 2);
                if (!focused) {
                    green(0.42f);
                    text(28, 54, "SYSTEM HAS INPUT FOCUS", 2);
                }
                if (hold > 0) {
                    green(0.55f);
                    text(368, 468, "RELEASE B TO CANCEL", 2);
                    arc(512, 384, 36, hold);
                }
            }
            glDisable(GL_SCISSOR_TEST);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (!XR_CHECK(xrReleaseSwapchainImage(swapchain, &release))) return false;
        ready = complete;
        lastDraw = now;
        return ready;
    }

    XrCompositionLayerQuad layer(XrSpace space, XrPosef pose) const {
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        // Pixels are premultiplied; leave UNPREMULTIPLIED_ALPHA unset.
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quad.space = space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = swapchain;
        quad.subImage.imageRect.extent = {width, height};
        quad.pose = pose;
        quad.size = controllerMode ? XrExtent2Df{0.10f, 0.10f} : XrExtent2Df{1.6f, 1.2f};
        return quad;
    }

private:
    int width = 1024, height = 768;
    bool controllerMode = false;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    GLuint fbo = 0;
    bool ready = false;
    int64_t lastDraw = 0, born = 0;
    void green(float alpha) { glClearColor(0.24f * alpha, 0.92f * alpha, 0.57f * alpha, alpha); }
    void rect(int x, int y, int w, int h) {
        glScissor(x, height - y - h, w, h);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    void arc(int cx, int cy, int radius, float fraction) {
        const int count = static_cast<int>(360 * fraction);
        for (int degree = 0; degree < count; ++degree) {
            float angle = degree * 0.01745329252f;
            rect(cx + static_cast<int>(sinf(angle) * radius),
                 cy - static_cast<int>(cosf(angle) * radius), 1, 1);
        }
    }
    void circle(int cx, int cy, int radius) { arc(cx, cy, radius, 1); }
    void ticks(int cx, int cy, int radius, int count) {
        for (int i = 0; i < count; ++i) {
            float angle = i * 6.28318530718f / count;
            for (int r = radius + 3; r < radius + (i % 3 == 0 ? 9 : 6); ++r)
                rect(cx + static_cast<int>(sinf(angle) * r),
                     cy - static_cast<int>(cosf(angle) * r), 1, 1);
        }
    }
    void text(int x, int y, const std::string& value, int scale) {
        // Five columns per glyph, least significant bit at the top.
        static const uint8_t letters[26][5] = {
            {126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},
            {127,65,65,34,28},{127,73,73,73,65},{127,9,9,9,1},
            {62,65,73,73,122},{127,8,8,8,127},{65,65,127,65,65},
            {32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
            {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},
            {127,9,9,9,6},{62,65,81,33,94},{127,9,25,41,70},
            {38,73,73,73,50},{1,1,127,1,1},{63,64,64,64,63},
            {31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},
            {3,4,120,4,3},{97,81,73,69,67}
        };
        static const uint8_t digits[10][5] = {
            {62,81,73,69,62},{0,66,127,64,0},{66,97,81,73,70},
            {33,65,69,75,49},{24,20,18,127,16},{39,69,69,69,57},
            {60,74,73,73,48},{1,113,9,5,3},{54,73,73,73,54},
            {6,73,73,41,30}
        };
        static const uint8_t dash[5] = {8,8,8,8,8}, blank[5] = {}, dot[5] = {0,96,96,0,0}, slash[5] = {64,32,16,8,4};
        for (char c : value) {
            const uint8_t* glyph = c >= 'A' && c <= 'Z' ? letters[c-'A'] :
                c >= '0' && c <= '9' ? digits[c-'0'] : c == '-' ? dash : c == '.' ? dot : c == '/' ? slash : blank;
            for (int col = 0; col < 5; ++col)
                for (int row = 0; row < 7; ++row)
                    if (glyph[col] & (1 << row))
                        rect(x + col*scale, y + row*scale, scale, scale);
            x += 6*scale;
        }
    }
};
