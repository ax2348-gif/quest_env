// Host probe of native/scene_client.h against a live rigid scene (robot_utils docs/scene3d.md): what the headset's
// AR view would get. g++ -std=c++17 -O2 -pthread -I native tools/scene_probe.cpp -o /tmp/scene_probe
// /tmp/scene_probe http://HOST:8080/nodes/sim/scene3d [seconds]
#include "scene_client.h"
#include <cstdio>
int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: scene_probe URL [seconds]\n");
        return 2;
    }
    double secs = argc > 2 ? std::atof(argv[2]) : 5;
    scene_client::Client c;
    c.start(argv[1]);
    uint32_t first = 0, last = 0;
    int64_t t0 = robot_ar::nowNs();
    scene_client::Snapshot s;
    while (robot_ar::nowNs() - t0 < int64_t(secs * 1e9)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        s = c.snapshot();
        if (s.frame) {
            if (!first)
                first = s.frame->seq;
            last = s.frame->seq;
        }
    }
    c.stop();
    if (!s.scene) {
        std::printf("no scene: %s%s\n", s.error.c_str(), s.unavailable ? " (unavailable)" : "");
        return 1;
    }
    size_t tris = 0, edges = 0;
    for (const auto &m : s.scene->meshes)
        tris += m.ntri(), edges += m.nedge();
    std::printf("scene %s: %zu bodies (%zu dynamic), %zu meshes, %zu tris, %zu edges; view %s / %s\n",
                s.scene->sceneId.c_str(), s.scene->bodies.size(), s.scene->dynamic.size(), s.scene->meshes.size(), tris,
                edges, s.view.style.c_str(), s.view.role.c_str());
    std::printf("frames: seq %u..%u (%.1f/s), last t %.3f, %zu colour overrides, error '%s'\n", first, last,
                (last - first) / secs, s.frame ? s.frame->t : -1, s.frame ? s.frame->colors.size() : 0, s.error.c_str());
    return s.frame ? 0 : 1;
}
