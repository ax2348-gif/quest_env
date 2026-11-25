// mesh_scene.h host tests: outline edges, primitives, matrices, and the rigid_scene.v1 contract against
// fixtures/rigid_scene (written by robot_utils' scene3d.format: index.json, blob.bin, frame.bin).
#include "mesh_scene.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>

using namespace mesh_scene;

static std::string slurp(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
static bool near(double a, double b, double tol = 1e-5) { return std::fabs(a - b) < tol; }

int main(int argc, char **argv) {
    const std::string dir = argc > 1 ? argv[1] : "fixtures/rigid_scene";
    // outline edges: a cube keeps its 12 edges, not its face diagonals
    Mesh box = boxMesh();
    assert(box.nvert() == 8 && box.ntri() == 12 && box.nedge() == 12);
    for (size_t i = 0; i < box.nedge(); i++) {
        const float *a = &box.v[box.edge[2 * i] * 3], *b = &box.v[box.edge[2 * i + 1] * 3];
        int differ = (a[0] != b[0]) + (a[1] != b[1]) + (a[2] != b[2]);
        assert(differ == 1);
    }
    assert(outlineEdges(box.v, box.tri, 100).empty());
    std::vector<uint32_t> two(box.tri.begin(), box.tri.begin() + 6);
    assert(outlineEdges(box.v, two, 30).size() == 8); // one face alone: its 4 boundary edges
    // primitives carry feature lines; a sphere has no creases of its own
    Mesh cyl = cylinderMesh(24), sph = sphereMesh(24), cap = capsuleMesh(2, 24);
    assert(cyl.nedge() == 24 * 2 + 4 && sph.nedge() == 24 + 4 * 12 && cap.nedge() == sph.nedge());
    float zmax = 0;
    for (size_t i = 0; i < cap.nvert(); i++)
        zmax = std::max(zmax, cap.v[i * 3 + 2]);
    assert(near(zmax, 3));
    // matrices: a quarter turn about z, then translation
    float pose[7] = {1, 2, 3, 0, 0, float(std::sin(M_PI / 4)), float(std::cos(M_PI / 4))};
    float p[3] = {1, 0, 0}, q[3];
    apply(fromPose(pose), p, q);
    assert(near(q[0], 1) && near(q[1], 3) && near(q[2], 3));
    apply(mul(fromPose(pose), scale(2, 2, 2)), p, q);
    assert(near(q[1], 4));
    // the scene: as robot_utils wrote it
    Scene s;
    std::string err;
    bool ok = parseScene(slurp(dir + "/index.json"), slurp(dir + "/blob.bin"), s, &err);
    if (!ok)
        std::fprintf(stderr, "parseScene: %s\n", err.c_str());
    assert(ok && s.schema == "rigid_scene.v1" && s.sceneId.size() == 16);
    assert(s.bodies.size() == 3 && s.dynamic.size() == 1 && s.dynamic[0] == 2);
    assert(s.bodies[1].name == "table" && !s.bodies[1].dynamic && near(s.bodies[1].pose[0], .5));
    assert(s.meshes.size() == 1 && s.meshes[0].nvert() == 8 && s.meshes[0].ntri() == 12 && s.meshes[0].nedge() == 12);
    assert(near(s.meshes[0].error, .001) && near(s.meshes[0].v[0], -.05));
    const Body &link = s.bodies[2];
    assert(link.geoms.size() == 2 && link.geoms[0].type == "mesh" && link.geoms[0].mesh == 0 &&
           link.geoms[0].role == "visual" && near(link.geoms[0].local[2], .1));
    assert(link.geoms[1].type == "capsule" && near(link.geoms[1].radius, .03) && near(link.geoms[1].halfLength, .1));
    Mat4 gm;
    assert(geomMatrix(s.bodies[1].geoms[0], gm) && near(gm.m[0], .3) && near(gm.m[5], .8));
    assert(!geomMatrix(s.bodies[0].geoms[0], gm)); // the plane
    // a frame: whole, then truncated, then garbage
    std::string fb = slurp(dir + "/frame.bin");
    Frame f;
    size_t used = decodeFrame(reinterpret_cast<const uint8_t *>(fb.data()), fb.size(), f);
    assert(used == fb.size() && used == 32 + 28 + 8 && f.seq == 42 && f.resetSeq == 3 && near(f.t, 1.25));
    assert(f.colors.size() == 1 && f.colors[0].slot == 0 && near(f.colors[0].rgba[1], 128 / 255.) &&
           near(f.colors[0].rgba[3], 102 / 255.));
    assert(f.sceneId == s.sceneId && f.poses.size() == 7 && near(f.poses[2], 3) && near(f.poses[6], .7071068));
    bool bad = false;
    assert(decodeFrame(reinterpret_cast<const uint8_t *>(fb.data()), fb.size() - 1, f, &bad) == 0 && !bad);
    std::string junk(40, 'x');
    assert(decodeFrame(reinterpret_cast<const uint8_t *>(junk.data()), junk.size(), f, &bad) == 0 && bad);
    // a blob too short is refused
    Scene t;
    assert(!parseScene(slurp(dir + "/index.json"), slurp(dir + "/blob.bin").substr(0, 50), t, &err));
    std::puts("mesh_scene: ok");
    return 0;
}
