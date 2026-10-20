#include "teleop_core.h"
#include <cassert>
#include <chrono>
#include <iostream>
using namespace teleop;
bool near(V a, V b, double tol = 1e-6) { return norm(a - b) < tol; }
bool near(M a, M b, double tol = 1e-6) {
    double sum = 0;
    for (int i = 0; i < 9; i++)
        sum += std::abs(a.v[i] - b.v[i]);
    return sum < tol;
}
Hand hand(V p = {}, M r = {}) {
    Hand h;
    h.valid = true;
    h.pose = {p, r};
    return h;
}
int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--json") {
        Intent in;
        Hand l = hand(), r = hand();
        in.update(10, true, l, r);
        l.grip = .9;
        in.update(10, true, l, r);
        l.pose.p.z = -.1;
        in.update(10, true, l, r);
        jsonIntent(std::cout, in);
        return 0;
    }

    assert(near(questToBase() * V{0, 0, -1}, {1, 0, 0}));
    assert(near(questToBase() * V{-1, 0, 0}, {0, 1, 0}));
    assert(near(questToBase() * V{0, 1, 0}, {0, 0, 1}));
    // Every physical axis retains its direction across arbitrary wrist anchors.
    for (int axis = 0; axis < 3; axis++)
        for (double heading : {0., pi / 2}) {
            Clutch c;
            Hand h = hand({.2, 1, -.4}, rotation(unit(V{1, 2, 3}), .9));
            c.update(h, true);
            h.grip = .8;
            c.update(h, true);
            assert(c.engaged);
            Pose anchor{{.4, .3, 1.2}, rotation({1, 0, 0}, .6)},
                base{{2, 3, 0}, rotation({0, 0, 1}, heading)};
            V dp;
            dp[axis] = .07;
            M dr = rotation(axis == 0 ? V{1, 0, 0} : axis == 1 ? V{0, 1, 0} : V{0, 0, 1}, .2);
            h.pose.p = h.pose.p + transpose(questToBase()) * dp;
            h.pose.r = transpose(questToBase()) * dr * questToBase() * h.pose.r;
            c.update(h, true);
            Pose goal = target(c, anchor, base);
            assert(near(goal.p, base.p + base.r * (anchor.p + dp)));
            assert(near(goal.r, base.r * dr * anchor.r));
        }
    // Held-at-start, hysteresis, tracking/focus/session/gap invalidation.
    Intent in;
    Hand l = hand(), r = hand();
    l.grip = .8;
    in.update(1, true, l, r);
    assert(!in.hands[0].engaged && in.hands[0].needsRelease);
    l.grip = 0;
    in.update(1, true, l, r);
    l.grip = .8;
    in.update(1, true, l, r);
    auto id = in.hands[0].id;
    assert(id == 1);
    l.grip = .5;
    in.update(1, true, l, r);
    assert(in.hands[0].engaged);
    in.update(1, false, l, r);
    in.update(1, true, l, r);
    assert(!in.hands[0].engaged);
    l.grip = 0;
    in.update(1, true, l, r);
    l.grip = .8;
    in.update(1, true, l, r);
    assert(in.hands[0].id == id + 1);
    in.update(2, true, l, r);
    assert(!in.hands[0].engaged);
    l.grip = 0;
    in.update(2, true, l, r);
    l.grip = .8;
    in.update(2, true, l, r, true);
    assert(in.hands[0].needsRelease);
    l.sx = 1;
    l.sy = 1;
    r.sx = 1;
    l.grip = 0;
    in.update(2, true, l, r);
    assert(near(in.drive, {.45, -.45, -.8}));
    in.update(2, false, l, r);
    assert(near(in.drive, {}));
    for (V axis : {V{1, 0, 0}, V{0, 1, 0}, unit(V{1, 2, 3})})
        assert(near(rotation(unit(logRotation(rotation(axis, pi))), norm(logRotation(rotation(axis, pi)))),
                    rotation(axis, pi), 1e-5));
    // Re-align to headset yaw without changing gravity or accepting a held clutch.
    {
        Intent intent;
        Hand left = hand(), right = hand();
        Pose head{{0, 1.6, 0}, rotation({0, 1, 0}, -pi / 2) * rotation({1, 0, 0}, -.3)};
        intent.update(77, true, left, right);
        left.grip = .8;
        intent.update(77, true, left, right);
        M previous = intent.operatorFromQuest;
        intent.update(77, true, left, right, false, &head, true);
        assert(intent.operatorFrameRevision == 0 && near(previous, intent.operatorFromQuest));
        assert(intent.hands[0].engaged); // A does not cancel an engaged clutch.
        left.grip = 0;
        intent.update(77, true, left, right);
        intent.update(77, true, left, right, false, &head, true);
        assert(intent.operatorFrameRevision == 1);
        assert(near(intent.operatorFromQuest * V{1, 0, 0}, V{1, 0, 0}));
        assert(near(intent.operatorFromQuest * V{0, 1, 0}, V{0, 0, 1}));
        intent.update(77, true, left, right);
        left.grip = .8;
        intent.update(77, true, left, right);
        left.pose.p.x = .12;
        intent.update(77, true, left, right);
        assert(near(intent.hands[0].delta, V{.12, 0, 0}));
        left.grip = 0;
        intent.update(77, true, left, right);
        head.r = rotation({1, 0, 0}, pi / 2); // heading undefined when looking straight up
        intent.update(77, true, left, right, false, &head, true);
        assert(intent.operatorFrameRevision == 1);
        intent.update(78, true, left, right);
        assert(intent.operatorFrameRevision == 0 && near(intent.operatorFromQuest, questToBase()));
    }
    // Ideal swerve velocities match the rigid-base twist for translation and yaw.
    {
        Robot robot;
        for (V twist : {V{.4, 0, 0}, V{0, .4, 0}, V{0, 0, .7}, V{.3, -.2, .5}, V{-.4, 0, 0}}) {
            for (auto &wheel : robot.wheels) {
                double before = wheel.phase;
                wheel.update(twist, .01);
                V actual{wheel.speed * std::cos(wheel.steering), wheel.speed * std::sin(wheel.steering), 0};
                assert(
                    near(actual, V{twist.x - twist.z * wheel.mount.y, twist.y + twist.z * wheel.mount.x, 0}));
                assert(std::abs(std::remainder(
                           wheel.phase - before + wheel.speed * .01 / SwerveModule::radius, 2 * pi)) < 1e-8);
                double angle = wheel.steering, phase = wheel.phase;
                wheel.update({}, .01);
                assert(wheel.speed == 0 && wheel.steering == angle && wheel.phase == phase);
            }
        }
        Arm arm;
        auto chain = arm.fk(arm.q);
        assert(std::abs(norm(chain.links[1] - chain.links[0]) - .36) < 1e-8);
        assert(std::abs(norm(chain.links[2] - chain.links[1]) - .336) < 1e-8);
    }
    // Analytic spatial Jacobian vs finite differences (position and orientation).
    for (int side : {1, -1}) {
        Arm a(side);
        Chain c = a.fk(a.q);
        double j[6][7];
        Arm::jacobian(c, j);
        for (int k = 0; k < 7; k++) {
            Q q = a.q;
            q[k] += 1e-6;
            auto other = a.fk(q);
            V p = (other.ee.p - c.ee.p) * 1e6, rot = logRotation(other.ee.r * transpose(c.ee.r)) * 1e6;
            for (int i = 0; i < 3; i++) {
                assert(std::abs(p[i] - j[i][k]) < 1e-5);
                assert(std::abs(.35 * rot[i] - j[i + 3][k]) < 1e-5);
            }
        }
    }
    // Reachable targets generated from a second joint posture test both pose parts.
    auto begin = std::chrono::steady_clock::now();
    for (int side : {1, -1})
        for (int axis = 0; axis < 7; axis++) {
            Arm a(side), other(-side);
            Q desired = a.q;
            desired[axis] += .16;
            Pose goal = a.fk(desired).ee;
            for (int step = 0; step < 180; step++)
                a.ik(goal, other.fk(other.q), 1. / 90);
            std::cout << "IK " << side << " axis " << axis << " pos " << a.posError << " rot " << a.rotError
                      << " guard " << a.limited << '\n';
            assert(a.posError < .008 && a.rotError < .025);
        }
    // Actual simulator: stationary hands + moving base: live follows, frozen holds.
    for (bool frozen : {false, true}) {
        Robot robot;
        robot.frozenMode = frozen;
        Intent intent;
        Hand left = hand(), right = hand();
        intent.update(42, true, left, right);
        robot.update(intent, 1. / 90, {0, 0, 1.6}, true, false);
        left.grip = right.grip = .8;
        intent.update(42, true, left, right);
        robot.update(intent, 1. / 90, {0, 0, 1.6}, true, false);
        Pose start = compose(robot.base, robot.arms[0].fk(robot.arms[0].q).ee);
        left.sy = .4;
        right.sx = .3;
        for (int n = 0; n < 45; n++) {
            intent.update(42, true, left, right);
            robot.update(intent, 1. / 90, {0, 0, 1.6}, true, false);
        }
        if (frozen)
            assert(near(robot.arms[0].targetWorld.p, start.p));
        else
            assert(norm(robot.arms[0].targetWorld.p - start.p) > .02);
        if (frozen) {
            Pose actual = compose(robot.base, robot.arms[0].fk(robot.arms[0].q).ee);
            assert(norm(actual.p - start.p) < .02);
        }
        left.grip = right.grip = 0;
        intent.update(42, true, left, right);
        Q held = robot.arms[0].q;
        for (int n = 0; n < 10; n++)
            robot.update(intent, 1. / 90, {0, 0, 1.6}, true, false);
        assert(robot.arms[0].q == held);
    }
    // Large unreachable command stays finite and within limits.
    Arm a, other(-1);
    for (int n = 0; n < 100; n++)
        a.ik({{3, 3, 3}, rotation({1, 0, 0}, pi)}, other.fk(other.q), 1. / 90);
    for (double q : a.q)
        assert(std::isfinite(q) && std::abs(q) <= 2.85);
    std::cout
        << "PASS control mapping, clutch validity, Jacobians, IK, live/frozen and release hold; elapsed "
        << std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() << "s\n";
}
