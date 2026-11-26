#pragma once
// Meta body tracking, forwarded as-is: the `body` block of each teleop.quest_state.v1
// record (schema teleop.quest_body.v1; BODY_TRACKING.md). This header is pure C++
// (no OpenXR, Android or GL) so host tests cover the configuration, the joint subsets
// and the JSON; telemetry.cpp owns the OpenXR calls (XR_FB_body_tracking,
// XR_META_body_tracking_full_body, XR_META_body_tracking_fidelity) and fills a Sample.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace body {

constexpr const char* kSchema = "teleop.quest_body.v1";
constexpr int kDefaultJointCount = 70;       // XR_BODY_JOINT_COUNT_FB
constexpr int kFullBodyJointCount = 84;      // XR_FULL_BODY_JOINT_COUNT_META
constexpr int32_t kJointSetDefault = 0;              // XR_BODY_JOINT_SET_DEFAULT_FB
constexpr int32_t kJointSetFullBody = 1000274000;    // XR_BODY_JOINT_SET_FULL_BODY_META

// `~/.config/quest-env/body-tracking`, written by `quest run --body=...`; absent = off.
// One `key=value` per line: joints=core|all, joint_set=full_body|default,
// fidelity=high|low|none, every=N (send the block on every Nth frame).
struct Config {
    bool enabled = false;
    bool allJoints = false;        // false: "core" (no finger joints)
    bool fullBody = true;          // ask for the full-body joint set (legs) when the runtime has it
    int fidelity = 2;              // 2 high, 1 low, 0 do not request (XrBodyTrackingFidelityMETA)
    int every = 1;
};

inline Config parseConfig(const std::string& text, bool present) {
    Config c;
    c.enabled = present;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "joints") c.allJoints = v == "all";
        else if (k == "joint_set") c.fullBody = v != "default";
        else if (k == "fidelity") c.fidelity = v == "low" ? 1 : v == "none" ? 0 : 2;
        else if (k == "every") { const int n = atoi(v.c_str()); c.every = n >= 1 && n <= 1000 ? n : 1; }
        else if (k == "enabled") c.enabled = v != "off" && v != "0";
    }
    return c;
}

// The joints sent for a joint set. "core" drops the 48 finger joints (the hand
// tracker's business) and keeps the root, spine, head, shoulders, scapulae, arms,
// wrist twists, both palms and wrists, and the full-body set's legs.
inline std::vector<int> jointSubset(int jointCount, bool all) {
    std::vector<int> out;
    for (int i = 0; i < jointCount; ++i) {
        const bool finger = (i >= 20 && i <= 43) || (i >= 46 && i <= 69);
        if (all || !finger) out.push_back(i);
    }
    return out;
}

inline const char* fidelityName(int f) { return f == 2 ? "high" : f == 1 ? "low" : nullptr; }

struct Joint {
    uint64_t flags = 0;            // XrSpaceLocationFlags: 1 orientation valid, 2 position valid,
                                   // 4 orientation tracked, 8 position tracked
    float p[3] = {0, 0, 0};        // LOCAL, metres
    float q[4] = {0, 0, 0, 1};     // x, y, z, w
};

// One frame's body state, filled by telemetry.cpp.
struct Sample {
    bool available = false;        // a tracker exists
    std::string reason;            // why not (when !available)
    bool active = false;           // XrBodyJointLocationsFB::isActive
    int32_t jointSet = kJointSetDefault;
    int jointCount = kDefaultJointCount;
    float confidence = 0;
    int fidelity = 0;              // reported (XrBodyTrackingFidelityStatusMETA); 0: not reported
    int fidelityRequested = 0;
    uint32_t skeletonChangedCount = 0;
    int64_t timeNs = 0;            // XrBodyJointLocationsFB::time
    bool allJoints = false;
    std::vector<Joint> joints;     // indexed by joint id, jointCount long when active
};

inline void num(std::ostream& o, float v, int precision) {
    if (!std::isfinite(v)) v = 0;
    char buf[32];
    snprintf(buf, sizeof buf, "%.*f", precision, static_cast<double>(v));
    // "-0.0000" -> "0.0000" keeps the output stable.
    o << (buf[0] == '-' && strtod(buf, nullptr) == 0.0 ? buf + 1 : buf);
}

// Appends `,"body":{...}`. Positions and quaternion components with 4 decimals
// (0.1 mm; 1e-4). A joint with non-finite values is sent with flags 0.
inline void appendJson(std::ostream& o, const Sample& s) {
    o << ",\"body\":{\"schema\":\"" << kSchema << "\",\"available\":" << (s.available ? "true" : "false");
    if (!s.available) {
        o << ",\"reason\":\"";
        for (char c : s.reason) {
            if (c == '"' || c == '\\') o << '\\';
            if (static_cast<unsigned char>(c) >= 0x20) o << c;
        }
        o << "\"}";
        return;
    }
    const char* fid = fidelityName(s.fidelity);
    const char* req = fidelityName(s.fidelityRequested);
    o << ",\"active\":" << (s.active ? "true" : "false")
      << ",\"joint_set\":\"" << (s.jointSet == kJointSetFullBody ? "full_body" : "default") << '"'
      << ",\"joint_set_id\":" << s.jointSet
      << ",\"joint_count\":" << s.jointCount
      << ",\"confidence\":";
    num(o, s.confidence, 3);
    o << ",\"fidelity\":" << (fid ? std::string("\"") + fid + "\"" : std::string("null"))
      << ",\"fidelity_requested\":" << (req ? std::string("\"") + req + "\"" : std::string("null"))
      << ",\"skeleton_changed_count\":" << s.skeletonChangedCount
      << ",\"time_ns\":" << s.timeNs
      << ",\"subset\":\"" << (s.allJoints ? "all" : "core") << '"';
    if (!s.active || static_cast<int>(s.joints.size()) < s.jointCount) {
        o << ",\"joints\":[],\"flags\":[],\"poses\":[]}";
        return;
    }
    const std::vector<int> ids = jointSubset(s.jointCount, s.allJoints);
    o << ",\"joints\":[";
    for (size_t i = 0; i < ids.size(); ++i) o << (i ? "," : "") << ids[i];
    o << "],\"flags\":[";
    for (size_t i = 0; i < ids.size(); ++i) {
        const Joint& j = s.joints[ids[i]];
        bool finite = true;
        for (float v : j.p) finite = finite && std::isfinite(v);
        for (float v : j.q) finite = finite && std::isfinite(v);
        o << (i ? "," : "") << (finite ? (j.flags & 0xF) : 0);
    }
    o << "],\"poses\":[";
    for (size_t i = 0; i < ids.size(); ++i) {
        const Joint& j = s.joints[ids[i]];
        for (int k = 0; k < 3; ++k) { if (i || k) o << ','; num(o, j.p[k], 4); }
        for (int k = 0; k < 4; ++k) { o << ','; num(o, j.q[k], 4); }
    }
    o << "]}";
}

} // namespace body
