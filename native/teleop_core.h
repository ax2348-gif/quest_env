#pragma once
// Pure kinematics and operator intent. No Android, OpenXR or robot dynamics.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>
namespace teleop {
constexpr double pi = 3.14159265358979323846;
inline double clamp(double x, double a, double b) { return std::max(a, std::min(b, x)); }
struct V {
    double x = 0, y = 0, z = 0;
    double &operator[](int i) { return i == 0 ? x : i == 1 ? y : z; }
    double operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
};
inline V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V operator*(V a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double norm(V a) { return std::sqrt(dot(a, a)); }
inline V unit(V a) { return a * (1 / std::max(1e-12, norm(a))); }
struct M {
    double v[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    double &operator()(int r, int c) { return v[r * 3 + c]; }
    double operator()(int r, int c) const { return v[r * 3 + c]; }
};
inline V operator*(const M &a, V b) {
    return {a(0, 0) * b.x + a(0, 1) * b.y + a(0, 2) * b.z, a(1, 0) * b.x + a(1, 1) * b.y + a(1, 2) * b.z,
            a(2, 0) * b.x + a(2, 1) * b.y + a(2, 2) * b.z};
}
inline M operator*(const M &a, const M &b) {
    M c;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            c(i, j) = 0;
            for (int k = 0; k < 3; k++)
                c(i, j) += a(i, k) * b(k, j);
        }
    return c;
}
inline M transpose(const M &a) {
    M b;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            b(i, j) = a(j, i);
    return b;
}
inline M rotation(V axis, double angle) {
    axis = unit(axis);
    double c = std::cos(angle), s = std::sin(angle);
    M r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r(i, j) = (i == j ? c : 0) + (1 - c) * axis[i] * axis[j];
    r(0, 1) -= s * axis.z;
    r(1, 0) += s * axis.z;
    r(0, 2) += s * axis.y;
    r(2, 0) -= s * axis.y;
    r(1, 2) -= s * axis.x;
    r(2, 1) += s * axis.x;
    return r;
}
inline V logRotation(const M &r) {
    double a = std::acos(clamp((r(0, 0) + r(1, 1) + r(2, 2) - 1) / 2, -1, 1));
    V skew = {r(2, 1) - r(1, 2), r(0, 2) - r(2, 0), r(1, 0) - r(0, 1)};
    if (a < 1e-6)
        return skew * .5;
    if (pi - a < 1e-4) {
        int k = 0;
        for (int i = 1; i < 3; i++)
            if (r(i, i) > r(k, k))
                k = i;
        V axis;
        axis[k] = std::sqrt(std::max(0., (r(k, k) + 1) / 2));
        for (int i = 0; i < 3; i++)
            if (i != k)
                axis[i] = (r(i, k) + r(k, i)) / (4 * std::max(axis[k], 1e-8));
        if (dot(axis, skew) < 0)
            axis = axis * -1;
        return unit(axis) * a;
    }
    return skew * (a / (2 * std::sin(a)));
}
struct Pose {
    V p;
    M r;
};
inline Pose compose(const Pose &a, const Pose &b) { return {a.p + a.r * b.p, a.r * b.r}; }
inline Pose inverse(const Pose &a) {
    M r = transpose(a.r);
    return {r * (a.p * -1), r};
}
inline M questToBase() { return M{{0, 0, -1, -1, 0, 0, 0, 1, 0}}; }
inline Pose operatorPose(const Pose &q) {
    M c = questToBase();
    return {c * q.p, c * q.r};
}
inline bool finite(const Pose &p) {
    for (double x : p.r.v)
        if (!std::isfinite(x))
            return false;
    return std::isfinite(p.p.x) && std::isfinite(p.p.y) && std::isfinite(p.p.z);
}
struct Hand {
    bool valid = false;
    Pose pose;
    double grip = 0, trigger = 0, sx = 0, sy = 0;
};
struct Clutch {
    bool engaged = false, needsRelease = true, valid = false;
    uint64_t id = 0;
    Pose anchor;
    V delta;
    M deltaR;
    double gripper = 0;
    void invalidate() {
        engaged = false;
        needsRelease = true;
        valid = false;
        delta = {};
        deltaR = {};
    }
    void update(const Hand &h, bool focused, const M &operatorFromQuest = questToBase()) {
        valid = focused && h.valid && finite(h.pose) && std::isfinite(h.grip);
        if (!valid) {
            invalidate();
            return;
        }
        gripper = clamp(h.trigger, 0, 1);
        if (h.grip <= .4) {
            engaged = false;
            needsRelease = false;
            delta = {};
            deltaR = {};
            return;
        }
        if (needsRelease)
            return;
        Pose current{operatorFromQuest * h.pose.p, operatorFromQuest * h.pose.r};
        if (!engaged && h.grip >= .6) {
            anchor = current;
            engaged = true;
            ++id;
        }
        if (engaged) {
            delta = current.p - anchor.p;
            double d = norm(delta);
            if (d > .45)
                delta = delta * (.45 / d);
            deltaR = current.r * transpose(anchor.r);
        }
    }
};
inline double stick(double x) { return std::isfinite(x) && std::abs(x) > .18 ? clamp(x, -1, 1) : 0; }
struct Intent {
    Clutch hands[2];
    V drive;
    int64_t session = 0;
    bool initialized = false;
    bool discontinuity = false;
    M operatorFromQuest = questToBase();
    uint64_t operatorFrameRevision = 0;
    bool realign(const Pose &head) {
        if (hands[0].engaged || hands[1].engaged || !finite(head))
            return false;
        V forward = questToBase() * (head.r * V{0, 0, -1});
        // Looking almost straight up/down does not define a reliable heading.
        if (std::hypot(forward.x, forward.y) < .15)
            return false;
        operatorFromQuest = rotation({0, 0, 1}, -std::atan2(forward.y, forward.x)) * questToBase();
        ++operatorFrameRevision;
        for (auto &h : hands)
            h.invalidate();
        drive = {};
        return true;
    }
    void update(int64_t s, bool focused, const Hand &left, const Hand &right, bool gap = false,
                const Pose *head = nullptr, bool align = false) {
        discontinuity = !initialized || s != session;
        if (discontinuity || gap) {
            for (auto &h : hands)
                h.invalidate();
        }
        if (discontinuity) {
            operatorFromQuest = questToBase();
            operatorFrameRevision = 0;
        }
        initialized = true;
        session = s;
        bool aligned =
            align && focused && !gap && left.grip <= .4 && right.grip <= .4 && head && realign(*head);
        hands[0].update(left, focused && !gap && !aligned, operatorFromQuest);
        hands[1].update(right, focused && !gap && !aligned, operatorFromQuest);
        drive = {};
        if (focused && !gap && !aligned) {
            if (hands[0].valid) {
                drive.x = .45 * stick(left.sy);
                drive.y = -.45 * stick(left.sx);
            }
            if (hands[1].valid)
                drive.z = -.8 * stick(right.sx);
        }
    }
};
// Controller delta is spatial in operator-base axes; EE anchor lives in robot base.
inline Pose target(const Clutch &c, const Pose &eeAnchor, const Pose &base) {
    return compose(base, {eeAnchor.p + c.delta, c.deltaR * eeAnchor.r});
}
constexpr double armScale = 1.2;
using Q = std::array<double, 7>;
struct Chain {
    Pose ee;
    std::array<V, 7> origins, axes;
    std::vector<V> links;
};
struct Arm {
    int side;
    Q q, rest;
    Pose anchor, frozen, targetWorld;
    uint64_t seen = 0;
    bool engaged = false;
    double posError = 0, rotError = 0, gripper = 0;
    bool limited = false;
    explicit Arm(int s = 1) : side(s) { reset(); }
    void reset() {
        rest = {side * .25, -.65, side * .15, 1.35, 0, -.7, 0};
        q = rest;
        engaged = false;
        seen = 0;
        posError = rotError = 0;
        gripper = 0;
        limited = false;
    }
    Chain fk(const Q &angles) const {
        static const V ax[7] = {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}, {0, 1, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        static const V offset[7] = {{0, 0, 0}, {0, 0, 0},    {.30, 0, 0}, {.28, 0, 0},
                                    {0, 0, 0}, {.065, 0, 0}, {.085, 0, 0}};
        Pose t{{0, side * .25, 1.03}, {}};
        Chain c;
        c.links.push_back(t.p);
        for (int i = 0; i < 7; i++) {
            c.origins[i] = t.p;
            c.axes[i] = t.r * ax[i];
            t.r = t.r * rotation(ax[i], angles[i]);
            t.p = t.p + t.r * (offset[i] * armScale);
            if (norm(offset[i]) > 0)
                c.links.push_back(t.p);
        }
        c.ee = t;
        return c;
    }
    static void jacobian(const Chain &c, double j[6][7]) {
        for (int k = 0; k < 7; k++) {
            V p = cross(c.axes[k], c.ee.p - c.origins[k]);
            for (int i = 0; i < 3; i++) {
                j[i][k] = p[i];
                j[i + 3][k] = .35 * c.axes[k][i];
            }
        }
    }
    static double solve(const double a[6][6], const double b[6], double x[6]) {
        double w[6][7];
        for (int i = 0; i < 6; i++) {
            for (int j = 0; j < 6; j++)
                w[i][j] = a[i][j];
            w[i][6] = b[i];
        }
        double logdet = 0;
        for (int i = 0; i < 6; i++) {
            int p = i;
            for (int k = i + 1; k < 6; k++)
                if (std::abs(w[k][i]) > std::abs(w[p][i]))
                    p = k;
            for (int j = i; j < 7; j++)
                std::swap(w[p][j], w[i][j]);
            double d = w[i][i];
            if (std::abs(d) < 1e-12)
                return -100;
            logdet += std::log(std::abs(d));
            for (int j = i; j < 7; j++)
                w[i][j] /= d;
            for (int k = 0; k < 6; k++)
                if (k != i) {
                    double f = w[k][i];
                    for (int j = i; j < 7; j++)
                        w[k][j] -= f * w[i][j];
                }
        }
        for (int i = 0; i < 6; i++) {
            x[i] = w[i][6];
        }
        return logdet;
    }
    double manipulability(const Q &v) const {
        double j[6][7], a[6][6] = {}, z[6] = {}, out[6] = {};
        jacobian(fk(v), j);
        for (int r = 0; r < 6; r++)
            for (int c = 0; c < 6; c++) {
                for (int k = 0; k < 7; k++)
                    a[r][c] += j[r][k] * j[c][k];
                if (r == c)
                    a[r][c] += .0001;
            }
        return solve(a, z, out);
    }
    // Capsule-distance guard for non-neighbor links, the torso, and the other arm.
    static double segmentDistance(V a, V b, V c, V d) {
        V u = b - a, v = d - c, w = a - c;
        double aa = dot(u, u), bb = dot(u, v), cc = dot(v, v), dd = dot(u, w), ee = dot(v, w);
        double s = 0, t = 0, den = aa * cc - bb * bb;
        if (den > 1e-12)
            s = clamp((bb * ee - cc * dd) / den, 0, 1);
        if (cc > 1e-12)
            t = (bb * s + ee) / cc;
        if (t < 0) {
            t = 0;
            s = aa > 1e-12 ? clamp(-dd / aa, 0, 1) : 0;
        }
        if (t > 1) {
            t = 1;
            s = aa > 1e-12 ? clamp((bb - dd) / aa, 0, 1) : 0;
        }
        return norm((a + u * s) - (c + v * t));
    }
    double clearance(const Chain &c, const Chain &other) const {
        double result = 10;
        for (size_t i = 1; i < c.links.size(); i++) {
            result = std::min(result,
                              segmentDistance(c.links[i - 1], c.links[i], {0, 0, .24}, {0, 0, 1.04}) - .13);
            for (size_t j = 1; j < other.links.size(); j++)
                result = std::min(
                    result, segmentDistance(c.links[i - 1], c.links[i], other.links[j - 1], other.links[j]) -
                                .065 * armScale);
            for (size_t j = i + 2; j < c.links.size(); j++)
                result =
                    std::min(result, segmentDistance(c.links[i - 1], c.links[i], c.links[j - 1], c.links[j]) -
                                         .055 * armScale);
            result = std::min(result, std::min(c.links[i - 1].z, c.links[i].z) - .20);
        }
        return result;
    }
    void ik(const Pose &goal, const Chain &other, double dt) {
        limited = false;
        for (int iteration = 0; iteration < 3; iteration++) {
            Chain c = fk(q);
            V dp = goal.p - c.ee.p, dr = logRotation(goal.r * transpose(c.ee.r));
            double j[6][7], a[6][6] = {}, e[6], x[6] = {};
            jacobian(c, j);
            for (int i = 0; i < 3; i++) {
                e[i] = dp[i];
                e[i + 3] = .35 * dr[i];
            }
            for (int r = 0; r < 6; r++)
                for (int col = 0; col < 6; col++) {
                    for (int k = 0; k < 7; k++)
                        a[r][col] += j[r][k] * j[col][k];
                    if (r == col)
                        a[r][col] += .0012;
                }
            solve(a, e, x);
            Q preference{}, dq{};
            for (int k = 0; k < 7; k++) {
                Q plus = q, minus = q;
                plus[k] += .005;
                minus[k] -= .005;
                double grad = (manipulability(plus) - manipulability(minus)) / .01;
                double limit = 2.85;
                preference[k] = .12 * (rest[k] - q[k]) + .018 * clamp(grad, -5, 5) -
                                .03 * q[k] / std::max(.15, limit * limit - q[k] * q[k]);
            }
            double jp[6] = {}, project[6] = {};
            for (int r = 0; r < 6; r++)
                for (int k = 0; k < 7; k++)
                    jp[r] += j[r][k] * preference[k];
            solve(a, jp, project);
            double maxStep = 0;
            for (int k = 0; k < 7; k++) {
                double main = 0, secondary = preference[k];
                for (int r = 0; r < 6; r++) {
                    main += j[r][k] * x[r];
                    secondary -= j[r][k] * project[r];
                }
                dq[k] = main + secondary * dt / 3;
                maxStep = std::max(maxStep, std::abs(dq[k]));
            }
            const double speed = 2.5 * dt / 3;
            double scale = maxStep > speed ? speed / maxStep : 1;
            double before = clearance(c, other);
            bool accepted = false;
            for (int attempt = 0; attempt < 7; attempt++) {
                Q trial = q;
                for (int k = 0; k < 7; k++)
                    trial[k] = clamp(q[k] + dq[k] * scale, -2.85, 2.85);
                if (clearance(fk(trial), other) >= std::min(.008, before - 1e-6)) {
                    q = trial;
                    accepted = true;
                    break;
                }
                scale *= .5;
            }
            if (!accepted) {
                limited = true;
                break;
            }
        }
        Chain c = fk(q);
        posError = norm(goal.p - c.ee.p);
        rotError = norm(logRotation(goal.r * transpose(c.ee.r)));
    }
};
struct SwerveModule {
    V mount;
    double steering = 0, speed = 0, phase = 0;
    static constexpr double radius = .11;
    void update(V twist, double dt) {
        V velocity{twist.x - twist.z * mount.y, twist.y + twist.z * mount.x, 0};
        speed = norm(velocity);
        if (speed < 1e-8) {
            speed = 0;
            return;
        }
        double desired = std::atan2(velocity.y, velocity.x);
        double delta = std::remainder(desired - steering, 2 * pi);
        if (std::abs(delta) > pi / 2) {
            delta = std::remainder(delta + pi, 2 * pi);
            speed = -speed;
        }
        steering = std::remainder(steering + delta, 2 * pi);
        phase = std::remainder(phase - speed * dt / radius, 2 * pi);
    }
};
struct Robot {
    Arm arms[2] = {Arm(1), Arm(-1)};
    Pose base;
    std::array<SwerveModule, 4> wheels{
        {{{.27, .27, 0}}, {{.27, -.27, 0}}, {{-.27, .27, 0}}, {{-.27, -.27, 0}}}};
    V ground;
    bool placed = false, frozenMode = false;
    int64_t session = 0;
    bool resetDown = false;
    void reset(V head, M heading = {}) {
        base = {head + heading * V{1.6, 0, 0} + V{0, 0, -1.6}, heading};
        for (auto &wheel : wheels) {
            wheel.steering = wheel.speed = wheel.phase = 0;
        }
        ground = base.p;
        for (auto &a : arms)
            a.reset();
        placed = true;
    }
    void update(const Intent &in, double dt, V head, bool headValid, bool resetButton) {
        if (!placed || session != in.session) {
            placed = false;
            if (headValid)
                reset(head, questToBase() * transpose(in.operatorFromQuest));
            session = in.session;
        }
        bool resetting = resetButton && !resetDown;
        resetDown = resetButton;
        if (resetting && headValid) {
            reset(head, questToBase() * transpose(in.operatorFromQuest));
            for (int i = 0; i < 2; i++)
                arms[i].seen = in.hands[i].id;
        }
        if (!placed)
            return;
        dt = clamp(dt, 0, .025);
        // Anchor from measured FK BEFORE moving the base on this engagement frame.
        for (int i = 0; i < 2; i++) {
            auto &a = arms[i];
            const auto &c = in.hands[i];
            if (c.valid)
                a.gripper = c.gripper;
            if (!c.engaged) {
                a.engaged = false;
                continue;
            }
            if (c.id != a.seen) {
                a.seen = c.id;
                a.anchor = a.fk(a.q).ee;
                a.frozen = base;
                a.engaged = true;
            }
        }
        for (auto &wheel : wheels)
            wheel.update(in.drive, dt);
        base.p = base.p +
                 base.r * rotation({0, 0, 1}, in.drive.z * dt * .5) * V{in.drive.x * dt, in.drive.y * dt, 0};
        base.r = base.r * rotation({0, 0, 1}, in.drive.z * dt);
        for (int i = 0; i < 2; i++) {
            auto &a = arms[i];
            const auto &c = in.hands[i];
            if (!c.engaged || !a.engaged)
                continue;
            a.targetWorld = target(c, a.anchor, frozenMode ? a.frozen : base);
            a.ik(compose(inverse(base), a.targetWorld), arms[1 - i].fk(arms[1 - i].q), dt);
        }
    }
};
inline void jsonV(std::ostream &o, V v) { o << '[' << v.x << ',' << v.y << ',' << v.z << ']'; }
inline void jsonM(std::ostream &o, const M &r) {
    o << '[';
    for (int i = 0; i < 9; i++) {
        if (i)
            o << ',';
        o << r.v[i];
    }
    o << ']';
}
inline void jsonIntent(std::ostream &o, const Intent &in) {
    o << "{\"schema\":\"teleop.operator_intent.v1\",\"session_id\":\"" << in.session
      << "\",\"frame\":\"operator_base_flu\",\"translation_limit_m\":0.45,\"base_velocity\":";
    jsonV(o, in.drive);
    o << ",\"operator_frame_revision\":" << in.operatorFrameRevision << ",\"operator_from_quest\":";
    jsonM(o, in.operatorFromQuest);
    o << ",\"hands\":{";
    for (int i = 0; i < 2; i++) {
        if (i)
            o << ',';
        const auto &c = in.hands[i];
        o << '"' << (i == 0 ? "left" : "right") << "\":{\"valid\":" << (c.valid ? "true" : "false")
          << ",\"engaged\":" << (c.engaged ? "true" : "false")
          << ",\"needs_release\":" << (c.needsRelease ? "true" : "false") << ",\"engagement_id\":" << c.id
          << ",\"delta_position\":";
        jsonV(o, c.delta);
        o << ",\"delta_rotation\":";
        jsonM(o, c.deltaR);
        o << ",\"gripper\":" << c.gripper << '}';
    }
    o << "}}";
}
} // namespace teleop
