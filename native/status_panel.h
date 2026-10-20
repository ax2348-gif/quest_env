// Small head-relative OpenXR status panel. No font assets or Java UI required.
class StatusPanel {
public:
    bool init(XrSession session) {
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

    bool draw(uint64_t frames, bool head, bool left, bool right,
              bool focused, bool connected, float hold) {
        // Update at 10 Hz; the compositor reuses the released image between updates.
        int64_t now = monotonicNs();
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
            glClearColor(0.025f, 0.04f, 0.065f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_SCISSOR_TEST);
            color(0.9f, 0.95f, 1);
            text(28, 24, "QUEST TELEMETRY", 5);
            color(head && focused ? 0.25f : 1.0f, 0.8f, 0.4f);
            text(28, 82, !focused ? "INPUT NOT FOCUSED" :
                 head ? "COLLECTING - HEAD TRACKED" : "COLLECTING - HEAD UNTRACKED", 3);
            color(0.7f, 0.8f, 0.9f);
            text(28, 122, "FRAMES " + std::to_string(frames), 3);
            text(28, 157, std::string("CONTROLLERS  L ") + (left ? "OK" : "--") +
                 "  R " + (right ? "OK" : "--"), 3);
            text(28, 192, connected ? "CLIENT CONNECTED" : "WAITING FOR CLIENT", 3);
            color(1, 0.65f, 0.4f);
            text(28, 244, "HOLD B 2 SECONDS TO SHUT DOWN", 3);
            if (hold > 0) rect(28, 282, static_cast<int>(hold * 700), 10);
            glDisable(GL_SCISSOR_TEST);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (!XR_CHECK(xrReleaseSwapchainImage(swapchain, &release))) return false;
        ready = complete;
        lastDraw = now;
        return ready;
    }

    XrCompositionLayerQuad layer(XrSpace view) const {
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        quad.space = view;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = swapchain;
        quad.subImage.imageRect.extent = {width, height};
        quad.pose.orientation.w = 1;
        quad.pose.position = {0, -0.22f, -1.25f};
        quad.size = {0.84f, 0.35f};
        return quad;
    }

private:
    static constexpr int width = 768, height = 320;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    GLuint fbo = 0;
    bool ready = false;
    int64_t lastDraw = 0;
    void color(float r, float g, float b) { glClearColor(r, g, b, 1); }
    void rect(int x, int y, int w, int h) {
        glScissor(x, height - y - h, w, h);
        glClear(GL_COLOR_BUFFER_BIT);
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
        static const uint8_t dash[5] = {8,8,8,8,8}, blank[5] = {};
        for (char c : value) {
            const uint8_t* glyph = c >= 'A' && c <= 'Z' ? letters[c-'A'] :
                c >= '0' && c <= '9' ? digits[c-'0'] : c == '-' ? dash : blank;
            for (int col = 0; col < 5; ++col)
                for (int row = 0; row < 7; ++row)
                    if (glyph[col] & (1 << row))
                        rect(x + col*scale, y + row*scale, scale, scale);
            x += 6*scale;
        }
    }
};
