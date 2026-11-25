#pragma once
// Background client of a rigid scene (robot_utils docs/scene3d.md): GET <base> (the index) and <base>/blob once
// per scene_id, then <base>/stream?format=bin frames; a frame with another scene_id fetches the scene again.
// Every second it also reads <base>/view ({style, role}: how the headset should draw it, set from a page).
// <base> is e.g. http://HOST:8080/nodes/sim/scene3d. The XR frame loop only takes snapshots of shared pointers.
#include "mesh_scene.h"
#include "robot_ar_client.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
namespace scene_client {
using robot_ar::Connection;
using robot_ar::nowNs;

struct View {
    std::string style = "hidden_line"; // wire | hidden_line | flat | flat_edges
    std::string role = "auto";         // auto (robot visual, the rest collision) | visual | collision
};
struct Snapshot {
    std::shared_ptr<const mesh_scene::Scene> scene;
    std::shared_ptr<const mesh_scene::Frame> frame;
    int64_t frameRxNs = 0;
    long sceneVersion = 0; // bumps on every newly fetched scene
    View view;
    bool connected = false;
    bool unavailable = false; // the server has no scene endpoint (404): use the description path instead
    std::string error;
};

inline bool parseView(const std::string &text, View &out) {
    robot_ar::Json j;
    if (!robot_ar::parseJson(text, j) || !j.isObj())
        return false;
    if (const auto *s = j.get("style"); s && s->isStr())
        out.style = s->s;
    if (const auto *r = j.get("role"); r && r->isStr())
        out.role = r->s;
    return true;
}

class Client {
  public:
    ~Client() { stop(); }
    void start(const std::string &base) {
        stop();
        base_ = base;
        while (!base_.empty() && base_.back() == '/')
            base_.pop_back();
        running_ = true;
        thread_ = std::thread([this] { run(); });
    }
    void stop() {
        running_ = false;
        if (thread_.joinable())
            thread_.join();
    }
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> g(mu_);
        return snap_;
    }

  private:
    void set(const std::function<void(Snapshot &)> &f) {
        std::lock_guard<std::mutex> g(mu_);
        f(snap_);
    }
    void error(const std::string &e) {
        set([&](Snapshot &s) {
            s.error = e;
            s.connected = false;
        });
    }
    bool fetchScene(std::string *sceneId) {
        std::string index, blob, err;
        if (!robot_ar::httpGet(base_, running_, index, err, 5000)) {
            set([&](Snapshot &s) {
                s.error = "SCENE " + err;
                s.connected = false;
                s.unavailable = err == "HTTP 404";
            });
            return false;
        }
        robot_ar::Json j;
        std::string id;
        if (robot_ar::parseJson(index, j))
            if (const auto *v = j.get("scene_id"); v && v->isStr())
                id = v->s;
        if (!robot_ar::httpGet(base_ + "/blob?scene_id=" + id, running_, blob, err, 10000)) {
            error("SCENE BLOB " + err);
            return false;
        }
        auto scene = std::make_shared<mesh_scene::Scene>();
        if (!mesh_scene::parseScene(index, blob, *scene, &err)) {
            error("SCENE " + err);
            return false;
        }
        *sceneId = scene->sceneId;
        set([&](Snapshot &s) {
            s.scene = scene;
            s.frame.reset();
            s.sceneVersion++;
            s.error.clear();
            s.unavailable = false;
        });
        return true;
    }
    void pollView(int64_t &next) {
        int64_t now = nowNs();
        if (now < next)
            return;
        next = now + 1000000000LL;
        std::string body, err;
        View v;
        if (robot_ar::httpGet(base_ + "/view", running_, body, err, 1000) && parseView(body, v))
            set([&](Snapshot &s) { s.view = v; });
    }
    // Stream frames until the scene changes (true: refetch) or the stream fails (false).
    bool stream(const std::string &sceneId, int64_t &nextView) {
        robot_ar::Url u;
        if (!robot_ar::parseUrl(base_ + "/stream?format=bin&hz=30", u)) {
            error("BAD SCENE URL");
            return false;
        }
        Connection c;
        if (!c.open(u, running_)) {
            error("SCENE STREAM " + c.error);
            return false;
        }
        std::string head, bytes, decoded;
        robot_ar::ResponseHead h;
        robot_ar::BodyDecoder d;
        bool inBody = false;
        char buf[16384];
        int idle = 0;
        while (running_) {
            pollView(nextView);
            ssize_t n = c.read(buf, sizeof(buf), 100);
            if (n == -2) {
                if ((idle += 100) >= 2000) {
                    error("SCENE STREAM STALLED");
                    return false;
                }
                continue;
            }
            idle = 0;
            if (n <= 0) {
                error(n == 0 ? "SCENE STREAM CLOSED" : "SCENE STREAM ERROR");
                return false;
            }
            decoded.clear();
            if (!inBody) {
                head.append(buf, size_t(n));
                size_t end = head.find("\r\n\r\n");
                if (end == std::string::npos)
                    continue;
                std::string rest = head.substr(end + 4);
                head.resize(end);
                if (!robot_ar::parseHead(head, h) || h.status != 200) {
                    error("SCENE STREAM HTTP " + std::to_string(h.status));
                    return false;
                }
                d.reset(h);
                inBody = true;
                if (!d.feed(rest.data(), rest.size(), decoded))
                    return false;
            } else if (!d.feed(buf, size_t(n), decoded)) {
                error("SCENE STREAM BAD CHUNKING");
                return false;
            }
            bytes += decoded;
            size_t at = 0;
            std::shared_ptr<mesh_scene::Frame> last;
            while (true) {
                auto f = std::make_shared<mesh_scene::Frame>();
                bool bad = false;
                size_t used = mesh_scene::decodeFrame(reinterpret_cast<const uint8_t *>(bytes.data()) + at,
                                                      bytes.size() - at, *f, &bad);
                if (bad) {
                    error("SCENE STREAM BAD FRAME");
                    return false;
                }
                if (!used)
                    break;
                at += used;
                last = f;
            }
            bytes.erase(0, at);
            if (last) {
                if (last->sceneId != sceneId)
                    return true; // a new scene: fetch it
                int64_t now = nowNs();
                set([&](Snapshot &s) {
                    s.frame = last;
                    s.frameRxNs = now;
                    s.connected = true;
                    s.error.clear();
                });
            }
        }
        return false;
    }
    void run() {
        int backoff = 250;
        int64_t nextView = 0;
        while (running_) {
            std::string id;
            pollView(nextView);
            if (fetchScene(&id)) {
                backoff = 250;
                while (running_ && stream(id, nextView) && fetchScene(&id)) {
                }
            }
            for (int waited = 0; running_ && waited < backoff; waited += 50)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            backoff = std::min(backoff * 2, snapshot().unavailable ? 10000 : 2000);
        }
    }
    std::string base_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mu_;
    Snapshot snap_;
};
} // namespace scene_client
