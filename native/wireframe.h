#pragma once
// World-space wireframe primitives as coloured line segments, in operator
// FLU coordinates (X forward, Y left, Z up). No OpenGL here: the stereo
// renderer converts segments to OpenXR LOCAL, and host tests inspect them.
#include "teleop_core.h"
namespace teleop {
struct Segment {
    V a, b, color;
};
struct Wire {
    std::vector<Segment> segs;
    void clear() { segs.clear(); }
    void line(V a, V b, V color) { segs.push_back({a, b, color}); }
    // Box with half extents, centred on the pose.
    void box(const Pose &p, V half, V color) {
        V v[8];
        for (int i = 0; i < 8; i++)
            v[i] = p.p + p.r * V{(i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y,
                                 (i & 4) ? half.z : -half.z};
        for (int i = 0; i < 8; i++)
            for (int k = 0; k < 3; k++)
                if (!(i & (1 << k)))
                    line(v[i], v[i | (1 << k)], color);
    }
    // Octagonal tube between two points (the demo's arm/mast links).
    void link(V a, V b, double radius, V color) {
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
    void axes(const Pose &p, double size) {
        line(p.p, p.p + p.r * V{size, 0, 0}, {1, .22, .18});
        line(p.p, p.p + p.r * V{0, size, 0}, {.25, 1, .4});
        line(p.p, p.p + p.r * V{0, 0, size}, {.3, .5, 1});
    }
    // Arc of radius r around c in the plane of unit vectors e1, e2, angles t0..t1.
    void arc(V c, V e1, V e2, double r, double t0, double t1, V color, int n = 8) {
        for (int i = 0; i < n; i++) {
            double a = t0 + (t1 - t0) * i / n, b = t0 + (t1 - t0) * (i + 1) / n;
            line(c + (e1 * std::cos(a) + e2 * std::sin(a)) * r, c + (e1 * std::cos(b) + e2 * std::sin(b)) * r,
                 color);
        }
    }
    void sphere(V c, double r, V color) {
        arc(c, {1, 0, 0}, {0, 1, 0}, r, 0, 2 * pi, color, 12);
        arc(c, {1, 0, 0}, {0, 0, 1}, r, 0, 2 * pi, color, 12);
        arc(c, {0, 1, 0}, {0, 0, 1}, r, 0, 2 * pi, color, 12);
    }
    // Capsule: the octagonal tube plus two hemispherical caps of crossed arcs.
    void capsule(V a, V b, double r, V color) {
        if (norm(b - a) < 1e-9) {
            sphere(a, r, color);
            return;
        }
        link(a, b, r, color);
        V along = unit(b - a);
        V u = unit(cross(along, std::abs(along.z) < .9 ? V{0, 0, 1} : V{0, 1, 0})), v = cross(along, u);
        for (V e : {u, v}) {
            arc(b, e, along, r, 0, pi, color, 6);
            arc(a, e, along * -1, r, 0, pi, color, 6);
        }
    }
    // Arrow in the horizontal plane from p along d (length = norm(d)).
    void arrow(V p, V d, V color) {
        double l = norm(d);
        if (l < 1e-6)
            return;
        V f = d * (1 / l), s = cross(V{0, 0, 1}, f);
        V tip = p + d;
        line(p, tip, color);
        double h = std::min(.08, l * .35);
        line(tip, tip - f * h + s * (h * .6), color);
        line(tip, tip - f * h - s * (h * .6), color);
    }
};
} // namespace teleop
