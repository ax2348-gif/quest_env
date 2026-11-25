#pragma once
// Rigid scenes (robot_utils docs/scene3d.md, rigid_scene.v1) on the headset, without GL: the scene index + mesh
// blob -> meshes and bodies, pose frames, primitives as meshes with feature lines, outline edges, matrices.
// Pure C++ so host tests cover it (test_mesh_scene.cpp); mesh_renderer.h draws the result.
//
// Scene: JSON index {schema, scene_id, bodies[{id, name, kind, dynamic, pose[7], geoms[...]}], dynamic[ids],
// meshes[{nvert, ntri, nedge, vert_offset, tri_offset, edge_offset, index u16|u32, error}]} + one blob.
// Frame: 32-byte little-endian header "RSF1", u32 seq, f64 t, u32 reset_seq, u16 n, u16 m, 8 bytes of scene_id,
// then n x 7 float32 (x, y, z, qx, qy, qz, qw) in `dynamic` order, then m colour overrides (u32 slot, 4 x u8 rgba):
// this frame, every geom of dynamic body `slot` in that colour (alpha < 1 see-through, 0 hidden). Conventions: z up,
// metres, quat xyzw.
#include "robot_ar.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
namespace mesh_scene {

// ---------------------------------------------------------------- matrices --
// Column-major 4x4 (GL convention): m[col * 4 + row].
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};
inline Mat4 mul(const Mat4 &a, const Mat4 &b) {
    Mat4 r;
    for (int c = 0; c < 4; c++)
        for (int w = 0; w < 4; w++) {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += a.m[k * 4 + w] * b.m[c * 4 + k];
            r.m[c * 4 + w] = s;
        }
    return r;
}
// Pose [x, y, z, qx, qy, qz, qw] -> matrix (the quaternion normalised; a zero one is the identity).
inline Mat4 fromPose(const float *p) {
    double x = p[3], y = p[4], z = p[5], w = p[6];
    double n = std::sqrt(x * x + y * y + z * z + w * w);
    Mat4 r;
    if (n > 1e-9) {
        x /= n, y /= n, z /= n, w /= n;
        const double R[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                             2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                             2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 3; col++)
                r.m[col * 4 + row] = float(R[row * 3 + col]);
    }
    r.m[12] = p[0], r.m[13] = p[1], r.m[14] = p[2];
    return r;
}
inline Mat4 scale(float sx, float sy, float sz) {
    Mat4 r;
    r.m[0] = sx, r.m[5] = sy, r.m[10] = sz;
    return r;
}
inline void apply(const Mat4 &a, const float *v, float *out) {
    for (int r = 0; r < 3; r++)
        out[r] = a.m[r] * v[0] + a.m[4 + r] * v[1] + a.m[8 + r] * v[2] + a.m[12 + r];
}

// ---------------------------------------------------------------- meshes ----
struct Mesh {
    std::string name;
    std::vector<float> v;       // x, y, z per vertex
    std::vector<uint32_t> tri;  // 3 per triangle
    std::vector<uint32_t> edge; // 2 per outline / feature edge
    double error = -1;          // m from the simulated surface; < 0 unknown
    size_t nvert() const { return v.size() / 3; }
    size_t ntri() const { return tri.size() / 3; }
    size_t nedge() const { return edge.size() / 2; }
};

// Boundary, non-manifold and crease edges (the two faces' normals more than creaseDeg apart): what a line
// drawing keeps. Same rule as robot_utils scene3d.format.outline_edges.
inline std::vector<uint32_t> outlineEdges(const std::vector<float> &v, const std::vector<uint32_t> &tri,
                                          double creaseDeg = 30) {
    struct E {
        uint32_t a, b, f;
    };
    std::vector<std::array<double, 3>> normal;
    std::vector<E> es;
    const size_t nf = tri.size() / 3;
    es.reserve(nf * 3);
    for (size_t f = 0; f < nf; f++) {
        const uint32_t *t = &tri[f * 3];
        double p[3][3];
        for (int k = 0; k < 3; k++)
            for (int c = 0; c < 3; c++)
                p[k][c] = v[t[k] * 3 + c];
        double u[3], w[3];
        for (int c = 0; c < 3; c++)
            u[c] = p[1][c] - p[0][c], w[c] = p[2][c] - p[0][c];
        std::array<double, 3> n{u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
        double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l < 1e-14)
            continue; // degenerate
        for (auto &x : n)
            x /= l;
        uint32_t fi = uint32_t(normal.size());
        normal.push_back(n);
        for (int k = 0; k < 3; k++) {
            uint32_t a = t[k], b = t[(k + 1) % 3];
            es.push_back({std::min(a, b), std::max(a, b), fi});
        }
    }
    std::sort(es.begin(), es.end(), [](const E &x, const E &y) { return x.a != y.a ? x.a < y.a : x.b < y.b; });
    const double cosMax = std::cos(creaseDeg * M_PI / 180);
    std::vector<uint32_t> out;
    for (size_t i = 0; i < es.size();) {
        size_t j = i;
        while (j < es.size() && es[j].a == es[i].a && es[j].b == es[i].b)
            j++;
        bool keep = j - i != 2;
        if (!keep) {
            const auto &n0 = normal[es[i].f], &n1 = normal[es[i + 1].f];
            keep = n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2] < cosMax;
        }
        if (keep)
            out.insert(out.end(), {es[i].a, es[i].b});
        i = j;
    }
    return out;
}

// ---------------------------------------------------------------- primitives -
// Unit primitives as meshes (scaled per geom). Smooth surfaces get feature lines in place of crease edges (a
// sphere has none), so a line drawing still shows their shape: rings and meridians.
inline Mesh boxMesh() { // half extents 1
    Mesh m;
    m.name = "box";
    for (int i = 0; i < 8; i++)
        m.v.insert(m.v.end(), {float(i & 1 ? 1 : -1), float(i & 2 ? 1 : -1), float(i & 4 ? 1 : -1)});
    const uint32_t q[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (auto &f : q)
        m.tri.insert(m.tri.end(), {f[0], f[1], f[2], f[0], f[2], f[3]});
    m.edge = outlineEdges(m.v, m.tri);
    m.error = 0;
    return m;
}
// A cylinder of radius 1 from z -1 to 1, `seg` sides, capped; feature lines: both rings and 4 rulings.
inline Mesh cylinderMesh(int seg = 24) {
    Mesh m;
    m.name = "cylinder";
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < seg; i++) {
            double a = 2 * M_PI * i / seg;
            m.v.insert(m.v.end(), {float(std::cos(a)), float(std::sin(a)), float(s ? 1 : -1)});
        }
    uint32_t c0 = uint32_t(m.nvert()), c1 = c0 + 1;
    m.v.insert(m.v.end(), {0, 0, -1, 0, 0, 1});
    for (int i = 0; i < seg; i++) {
        uint32_t a = i, b = (i + 1) % seg, A = a + seg, B = b + seg;
        m.tri.insert(m.tri.end(), {a, b, B, a, B, A, c0, b, a, c1, A, B});
        m.edge.insert(m.edge.end(), {a, b, A, B});
        if (i % (seg / 4) == 0)
            m.edge.insert(m.edge.end(), {a, A});
    }
    m.error = 1 - std::cos(M_PI / seg); // chord error at radius 1
    return m;
}
// A sphere of radius 1 (latitude-longitude); feature lines: the equator and two meridian circles.
inline Mesh sphereMesh(int seg = 24) {
    Mesh m;
    m.name = "sphere";
    const int rings = seg / 2;
    for (int r = 0; r <= rings; r++) {
        double th = M_PI * r / rings;
        for (int i = 0; i < seg; i++) {
            double a = 2 * M_PI * i / seg;
            m.v.insert(m.v.end(), {float(std::sin(th) * std::cos(a)), float(std::sin(th) * std::sin(a)),
                                   float(std::cos(th))});
        }
    }
    auto at = [&](int r, int i) { return uint32_t(r * seg + (i % seg)); };
    for (int r = 0; r < rings; r++)
        for (int i = 0; i < seg; i++) {
            m.tri.insert(m.tri.end(), {at(r, i), at(r + 1, i), at(r + 1, i + 1), at(r, i), at(r + 1, i + 1),
                                       at(r, i + 1)});
            if (2 * r == rings)
                m.edge.insert(m.edge.end(), {at(r, i), at(r, i + 1)});
            if (i % (seg / 4) == 0)
                m.edge.insert(m.edge.end(), {at(r, i), at(r + 1, i)});
        }
    m.error = 1 - std::cos(M_PI / seg);
    return m;
}
// A capsule's two hemispheres and its side for radius 1 and half length h (no scaling: the caps must stay round).
inline Mesh capsuleMesh(double h, int seg = 24) {
    Mesh m = sphereMesh(seg);
    m.name = "capsule";
    for (size_t i = 0; i < m.nvert(); i++)
        m.v[i * 3 + 2] += float(m.v[i * 3 + 2] >= 0 ? h : -h); // split at the equator: the side is its band
    return m;
}

// ---------------------------------------------------------------- scene -----
struct Geom {
    std::string type, role;
    float local[7] = {0, 0, 0, 0, 0, 0, 1};
    float rgba[4] = {.7f, .7f, .7f, 1};
    float half[3] = {0, 0, 0}; // box / plane half extents, ellipsoid radii
    float radius = 0, halfLength = 0;
    int mesh = -1;
};
struct Body {
    int id = -1;
    std::string name, kind;
    bool dynamic = false;
    float pose[7] = {0, 0, 0, 0, 0, 0, 1};
    std::vector<Geom> geoms;
};
struct Scene {
    std::string schema, sceneId;
    std::vector<Body> bodies;
    std::vector<int> dynamic;
    std::vector<Mesh> meshes;
};
struct Frame {
    uint32_t seq = 0, resetSeq = 0;
    double t = 0;
    std::string sceneId; // 16 hex digits
    std::vector<float> poses; // 7 per dynamic body
    struct Color {
        uint32_t slot;
        float rgba[4];
    };
    std::vector<Color> colors; // this frame's overrides
};

namespace detail {
inline bool floats(const robot_ar::Json *j, float *out, size_t n) {
    if (!j || !j->numbers(n))
        return false;
    for (size_t i = 0; i < n; i++)
        out[i] = float(j->a[i].n);
    return true;
}
inline std::string str(const robot_ar::Json *j) { return j && j->isStr() ? j->s : ""; }
inline double num(const robot_ar::Json *j, double d) { return j && j->isNum() ? j->n : d; }
template <typename T> void readIndices(const uint8_t *p, size_t count, std::vector<uint32_t> &out) {
    out.resize(count);
    for (size_t i = 0; i < count; i++) {
        T x;
        std::memcpy(&x, p + i * sizeof(T), sizeof(T)); // little-endian host (arm64, x86-64)
        out[i] = x;
    }
}
} // namespace detail

inline bool parseScene(const std::string &indexJson, const std::string &blob, Scene &out,
                       std::string *error = nullptr) {
    using robot_ar::Json;
    auto fail = [&](const std::string &why) {
        if (error)
            *error = why;
        return false;
    };
    Json j;
    if (!robot_ar::parseJson(indexJson, j) || !j.isObj())
        return fail("scene index is not a JSON object");
    Scene s;
    s.schema = detail::str(j.get("schema"));
    if (s.schema != "rigid_scene.v1")
        return fail("schema is not rigid_scene.v1: " + s.schema);
    s.sceneId = detail::str(j.get("scene_id"));
    if (const Json *ms = j.get("meshes"); ms && ms->isArr())
        for (const auto &mj : ms->a) {
            Mesh m;
            m.name = detail::str(mj.get("name"));
            size_t nv = size_t(detail::num(mj.get("nvert"), 0)), nt = size_t(detail::num(mj.get("ntri"), 0)),
                   ne = size_t(detail::num(mj.get("nedge"), 0));
            size_t vo = size_t(detail::num(mj.get("vert_offset"), -1)), to = size_t(detail::num(mj.get("tri_offset"), -1)),
                   eo = size_t(detail::num(mj.get("edge_offset"), -1));
            bool u32 = detail::str(mj.get("index")) == "u32";
            size_t is = u32 ? 4 : 2;
            if (vo + nv * 12 > blob.size() || to + nt * 3 * is > blob.size() || eo + ne * 2 * is > blob.size())
                return fail("mesh " + m.name + " runs past the blob");
            m.v.resize(nv * 3);
            std::memcpy(m.v.data(), blob.data() + vo, nv * 12);
            const auto *b = reinterpret_cast<const uint8_t *>(blob.data());
            if (u32) {
                detail::readIndices<uint32_t>(b + to, nt * 3, m.tri);
                detail::readIndices<uint32_t>(b + eo, ne * 2, m.edge);
            } else {
                detail::readIndices<uint16_t>(b + to, nt * 3, m.tri);
                detail::readIndices<uint16_t>(b + eo, ne * 2, m.edge);
            }
            for (uint32_t x : m.tri)
                if (x >= nv)
                    return fail("mesh " + m.name + ": triangle index out of range");
            for (uint32_t x : m.edge)
                if (x >= nv)
                    return fail("mesh " + m.name + ": edge index out of range");
            const Json *e = mj.get("error");
            m.error = e && e->isNum() ? e->n : -1;
            s.meshes.push_back(std::move(m));
        }
    const Json *bs = j.get("bodies");
    if (!bs || !bs->isArr())
        return fail("scene has no bodies");
    for (const auto &bj : bs->a) {
        Body b;
        b.id = int(detail::num(bj.get("id"), -1));
        b.name = detail::str(bj.get("name"));
        b.kind = detail::str(bj.get("kind"));
        const Json *d = bj.get("dynamic");
        b.dynamic = d && d->type == Json::Bool && d->b;
        detail::floats(bj.get("pose"), b.pose, 7);
        if (const Json *gs = bj.get("geoms"); gs && gs->isArr())
            for (const auto &gj : gs->a) {
                Geom g;
                g.type = detail::str(gj.get("type"));
                g.role = detail::str(gj.get("role"));
                detail::floats(gj.get("pos"), g.local, 3);
                detail::floats(gj.get("quat"), g.local + 3, 4);
                if (!detail::floats(gj.get("rgba"), g.rgba, 4))
                    detail::floats(gj.get("rgba"), g.rgba, 3);
                if (!detail::floats(gj.get("half_extents"), g.half, 3))
                    detail::floats(gj.get("half_extents"), g.half, 2);
                detail::floats(gj.get("radii"), g.half, 3);
                g.radius = float(detail::num(gj.get("radius"), 0));
                g.halfLength = float(detail::num(gj.get("half_length"), 0));
                g.mesh = int(detail::num(gj.get("mesh"), -1));
                if (g.type == "mesh" && (g.mesh < 0 || g.mesh >= int(s.meshes.size())))
                    return fail("body " + b.name + ": geom mesh index out of range");
                b.geoms.push_back(g);
            }
        if (b.id != int(s.bodies.size()))
            return fail("body ids are not 0..n-1 in order");
        s.bodies.push_back(std::move(b));
    }
    if (const Json *dy = j.get("dynamic"); dy && dy->isArr())
        for (const auto &x : dy->a) {
            int i = int(detail::num(&x, -1));
            if (i < 0 || i >= int(s.bodies.size()))
                return fail("dynamic body id out of range");
            s.dynamic.push_back(i);
        }
    out = std::move(s);
    return true;
}

constexpr size_t kFrameHeader = 32;
// One frame from the start of `data`: bytes used, or 0 if it is not a whole frame yet (or not a frame: *bad).
inline size_t decodeFrame(const uint8_t *data, size_t size, Frame &out, bool *bad = nullptr) {
    if (bad)
        *bad = false;
    if (size < kFrameHeader)
        return 0;
    if (std::memcmp(data, "RSF1", 4) != 0) {
        if (bad)
            *bad = true;
        return 0;
    }
    uint32_t seq, rs;
    uint16_t n, m;
    double t;
    std::memcpy(&seq, data + 4, 4);
    std::memcpy(&t, data + 8, 8);
    std::memcpy(&rs, data + 16, 4);
    std::memcpy(&n, data + 20, 2);
    std::memcpy(&m, data + 22, 2);
    size_t need = kFrameHeader + size_t(n) * 28 + size_t(m) * 8;
    if (size < need)
        return 0;
    static const char *hex = "0123456789abcdef";
    out.sceneId.clear();
    for (int i = 0; i < 8; i++)
        out.sceneId += hex[data[24 + i] >> 4], out.sceneId += hex[data[24 + i] & 15];
    out.seq = seq, out.t = t, out.resetSeq = rs;
    out.poses.resize(size_t(n) * 7);
    std::memcpy(out.poses.data(), data + kFrameHeader, size_t(n) * 28);
    out.colors.clear();
    for (size_t i = 0; i < m; i++) {
        const uint8_t *c = data + kFrameHeader + size_t(n) * 28 + i * 8;
        Frame::Color col;
        std::memcpy(&col.slot, c, 4);
        for (int k = 0; k < 4; k++)
            col.rgba[k] = c[4 + k] / 255.f;
        if (col.slot < n)
            out.colors.push_back(col);
    }
    return need;
}

// A geom's matrix in its body: its local pose and, for primitives, the scale that takes the unit mesh to its
// size. Capsules are not scaled along z (capsuleMesh is built for its half length); planes are skipped (false).
inline bool geomMatrix(const Geom &g, Mat4 &out) {
    Mat4 l = fromPose(g.local);
    if (g.type == "box")
        out = mul(l, scale(g.half[0], g.half[1], g.half[2]));
    else if (g.type == "ellipsoid")
        out = mul(l, scale(g.half[0], g.half[1], g.half[2]));
    else if (g.type == "sphere")
        out = mul(l, scale(g.radius, g.radius, g.radius));
    else if (g.type == "cylinder")
        out = mul(l, scale(g.radius, g.radius, g.halfLength));
    else if (g.type == "capsule")
        out = mul(l, scale(g.radius, g.radius, g.radius)); // its mesh carries halfLength / radius
    else if (g.type == "mesh")
        out = l;
    else
        return false;
    return true;
}

} // namespace mesh_scene
