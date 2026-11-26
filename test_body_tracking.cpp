// Host test for native/body_tracking.h: prints one test record per line
// ({"case":...} + the body block); test_body_tracking.py checks them and the
// golden fixtures/body_frames.jsonl that robot_utils' reader tests read.
#include "body_tracking.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <sstream>

using namespace body;

static Sample makeSample(int count, int32_t set, bool all) {
    Sample s;
    s.available = true;
    s.active = true;
    s.jointSet = set;
    s.jointCount = count;
    s.confidence = 0.875f;
    s.fidelity = 2;
    s.fidelityRequested = 2;
    s.skeletonChangedCount = 3;
    s.timeNs = 123456789012345LL;
    s.allJoints = all;
    s.joints.resize(count);
    for (int i = 0; i < count; ++i) {
        Joint& j = s.joints[i];
        j.flags = (i % 5 == 4) ? 0x3 : 0xF;       // every fifth joint valid but not tracked
        j.p[0] = 0.01f * i; j.p[1] = 1.5f - 0.02f * i; j.p[2] = -0.001f * i;
        const float a = 0.01f * i;
        j.q[0] = 0; j.q[1] = std::sin(a / 2); j.q[2] = 0; j.q[3] = std::cos(a / 2);
    }
    return s;
}

static void print(const char* name, const Sample& s) {
    std::ostringstream o;
    o << "{\"case\":\"" << name << '"';
    appendJson(o, s);
    o << '}';
    std::cout << o.str() << '\n';
}

int main() {
    // Config parsing.
    Config c = parseConfig("joints=all\njoint_set=default\nfidelity=low\nevery=3\n", true);
    assert(c.enabled && c.allJoints && !c.fullBody && c.fidelity == 1 && c.every == 3);
    c = parseConfig("", false);
    assert(!c.enabled && !c.allJoints && c.fullBody && c.fidelity == 2 && c.every == 1);
    c = parseConfig("every=0\nfidelity=none\n", true);
    assert(c.every == 1 && c.fidelity == 0);
    // Subsets: core drops the 48 finger joints (20..43, 46..69), keeps palms and wrists.
    assert(jointSubset(kDefaultJointCount, true).size() == 70);
    assert(jointSubset(kDefaultJointCount, false).size() == 22);
    assert(jointSubset(kFullBodyJointCount, false).size() == 36);
    assert(jointSubset(kFullBodyJointCount, false).back() == 83);

    print("default_core", makeSample(kDefaultJointCount, kJointSetDefault, false));
    print("default_all", makeSample(kDefaultJointCount, kJointSetDefault, true));
    print("full_body_core", makeSample(kFullBodyJointCount, kJointSetFullBody, false));
    Sample inactive = makeSample(kDefaultJointCount, kJointSetDefault, false);
    inactive.active = false;
    inactive.confidence = 0;
    inactive.fidelity = 0;
    print("inactive", inactive);
    Sample bad = makeSample(kDefaultJointCount, kJointSetDefault, false);
    bad.joints[11].p[0] = NAN;              // left arm lower: non-finite -> flags 0, zeros
    bad.joints[16].q[3] = -0.0f;            // -0 prints as 0
    bad.fidelityRequested = 0;
    bad.fidelity = 0;
    print("nonfinite", bad);
    Sample off;
    off.reason = "xrCreateBodyTrackerFB failed (-1000710001); BODY_TRACKING permission \"not granted\"";
    print("unavailable", off);
    return 0;
}
