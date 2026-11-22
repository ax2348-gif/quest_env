#pragma once
// Robot AR view: the robot description + robot_state.v1 -> forward kinematics ->
// wireframe primitives placed in the operator's space. Pure C++ (no network,
// OpenXR or GL) so host tests cover parsing, FK, placement and staleness.
//
// Inputs (see ROBOT_AR.md):
//   description  GET .../nodes/description/model/tree?variant=boxes (falls back to simple)
//   state        robot_state.v1 JSON lines: {"t","seq","model_version",
//                "base":[x,y,z,qx,qy,qz,qw], "joints":{name: q}, "overlays":{...},
//                "objects":{name:{"pose":[7],"geom":{...}}}}
// Sim world: z up, metres. Operator space: FLU of OpenXR LOCAL (teleop_core.h).
#include "wireframe.h"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
namespace robot_ar {
using teleop::M;
using teleop::Pose;
using teleop::V;

// ---------------------------------------------------------------- JSON ------
struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;
    const Json *get(const std::string &key) const {
        if (type != Obj)
            return nullptr;
        for (const auto &kv : o)
            if (kv.first == key)
                return &kv.second;
        return nullptr;
    }
    bool isNum() const { return type == Num; }
    bool isStr() const { return type == Str; }
    bool isObj() const { return type == Obj; }
    bool isArr() const { return type == Arr; }
    // Array of exactly `count` numbers (count 0 = any length).
    bool numbers(size_t count = 0) const {
        if (type != Arr || (count && a.size() != count) || a.empty())
            return false;
        for (const auto &x : a)
            if (x.type != Num)
                return false;
        return true;
    }
};
class JsonParser {
    const std::string &t;
    size_t i = 0;
    int depth = 0;
    void ws() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r'))
            i++;
    }
    bool lit(const char *w) {
        size_t n = std::strlen(w);
        if (t.compare(i, n, w) != 0)
            return false;
        i += n;
        return true;
    }
    static void utf8(std::string &out, unsigned c) {
        if (c < 0x80)
            out += char(c);
        else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    bool str(std::string &out) {
        if (i >= t.size() || t[i] != '"')
            return false;
        i++;
        while (i < t.size()) {
            char c = t[i++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= t.size())
                return false;
            char e = t[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (i + 4 > t.size())
                    return false;
                unsigned c = 0;
                for (int k = 0; k < 4; k++) {
                    char h = t[i++];
                    c <<= 4;
                    if (h >= '0' && h <= '9')
                        c |= h - '0';
                    else if (h >= 'a' && h <= 'f')
                        c |= h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F')
                        c |= h - 'A' + 10;
                    else
                        return false;
                }
                utf8(out, c); // Surrogate pairs are kept as two code units; names are ASCII.
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool value(Json &v) {
        if (++depth > 64)
            return false;
        ws();
        if (i >= t.size())
            return false;
        char c = t[i];
        bool ok = false;
        if (c == '{') {
            v.type = Json::Obj;
            i++;
            ws();
            if (i < t.size() && t[i] == '}') {
                i++;
                ok = true;
            } else
                while (true) {
                    ws();
                    std::pair<std::string, Json> kv;
                    if (!str(kv.first))
                        break;
                    ws();
                    if (i >= t.size() || t[i] != ':')
                        break;
                    i++;
                    if (!value(kv.second))
                        break;
                    v.o.push_back(std::move(kv));
                    ws();
                    if (i < t.size() && t[i] == ',') {
                        i++;
                        continue;
                    }
                    if (i < t.size() && t[i] == '}') {
                        i++;
                        ok = true;
                    }
                    break;
                }
        } else if (c == '[') {
            v.type = Json::Arr;
            i++;
            ws();
            if (i < t.size() && t[i] == ']') {
                i++;
                ok = true;
            } else
                while (true) {
                    Json x;
                    if (!value(x))
                        break;
                    v.a.push_back(std::move(x));
                    ws();
                    if (i < t.size() && t[i] == ',') {
                        i++;
                        continue;
                    }
                    if (i < t.size() && t[i] == ']') {
                        i++;
                        ok = true;
                    }
                    break;
                }
        } else if (c == '"') {
            v.type = Json::Str;
            ok = str(v.s);
        } else if (lit("true")) {
            v.type = Json::Bool;
            v.b = ok = true;
        } else if (lit("false")) {
            v.type = Json::Bool;
            ok = true;
        } else if (lit("null")) {
            ok = true;
        } else if (lit("NaN") || lit("Infinity") || lit("-Infinity")) {
            ok = true; // Python's json emits these; treat as null.
        } else {
            const char *start = t.c_str() + i;
            char *end = nullptr;
            v.n = std::strtod(start, &end);
            if (end != start) {
                v.type = Json::Num;
                i += end - start;
                ok = true;
            }
        }
        depth--;
        return ok;
    }

  public:
    explicit JsonParser(const std::string &text) : t(text) {}
    bool parse(Json &out) {
        if (!value(out))
            return false;
        ws();
        return i == t.size();
    }
};
inline bool parseJson(const std::string &text, Json &out) {
    out = Json();
    return JsonParser(text).parse(out);
}
inline double num(const Json *j, double fallback) { return j && j->isNum() ? j->n : fallback; }

// ------------------------------------------------------------ geometry ------
inline M quat(double x, double y, double z, double w) {
    double n = std::sqrt(x * x + y * y + z * z + w * w);
    if (n < 1e-12)
        return {};
    x /= n, y /= n, z /= n, w /= n;
    return M{{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 2 * (x * y + z * w),
              1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 2 * (x * z - y * w), 2 * (y * z + x * w),
              1 - 2 * (x * x + y * y)}};
}
// URDF fixed-axis roll, pitch, yaw: R = Rz(yaw) Ry(pitch) Rx(roll).
inline M rpy(double r, double p, double y) {
    return teleop::rotation({0, 0, 1}, y) * teleop::rotation({0, 1, 0}, p) * teleop::rotation({1, 0, 0}, r);
}
inline bool vec3(const Json *j, V &out) {
    if (!j || !j->numbers(3))
        return false;
    out = {j->a[0].n, j->a[1].n, j->a[2].n};
    return true;
}
// [x,y,z,qx,qy,qz,qw], or {pos|xyz|position, quat|quaternion (xyzw) | rpy}, or an
// object holding such a pose under "origin" / "pose".
inline bool parsePose(const Json *j, Pose &out) {
    if (!j)
        return false;
    if (j->numbers(7)) {
        out.p = {j->a[0].n, j->a[1].n, j->a[2].n};
        out.r = quat(j->a[3].n, j->a[4].n, j->a[5].n, j->a[6].n);
        return true;
    }
    if (!j->isObj())
        return false;
    bool any = false;
    Pose p;
    for (const char *k : {"pos", "xyz", "position"})
        if (vec3(j->get(k), p.p)) {
            any = true;
            break;
        }
    const Json *q = j->get("quat");
    if (!q)
        q = j->get("quaternion");
    V e;
    if (q && q->numbers(4)) {
        p.r = quat(q->a[0].n, q->a[1].n, q->a[2].n, q->a[3].n);
        any = true;
    } else if (vec3(j->get("rpy"), e)) {
        p.r = rpy(e.x, e.y, e.z);
        any = true;
    }
    if (any) {
        out = p;
        return true;
    }
    for (const char *k : {"origin", "pose"})
        if (const Json *nested = j->get(k))
            if (nested != j && parsePose(nested, out))
                return true;
    return false;
}

struct Visual {
    enum Kind { Box, Cylinder, Capsule, Sphere } kind = Box;
    Pose local;          // In the link (or object) frame. Cylinder/capsule axis = local z.
    V half;              // Box half extents.
    double radius = 0;   // Sphere, cylinder, capsule.
    double halfLength = 0; // Cylinder / capsule straight part, half length.
    V rgb{.65, .8, .77};
    bool coloured = false;
};
// One visual. Sizes: box "size" = full extents (URDF) or "half_size"/"half_extents";
// sphere "radius"; cylinder/capsule "radius" + "length" (full straight length, URDF)
// or MuJoCo-style "size": [radius, half_length]. Meshes are skipped (false).
inline bool parseVisual(const Json &v, Visual &out) {
    if (!v.isObj())
        return false;
    const Json *g = v.get("geometry");
    if (!g || !g->isObj())
        g = &v;
    auto typeOf = [](const Json *src) -> const Json * {
        for (const char *k : {"type", "shape", "geom", "kind"}) {
            const Json *t = src->get(k);
            if (t && t->isStr())
                return t;
        }
        return nullptr;
    };
    const Json *type = typeOf(g);
    if (!type && g != &v)
        type = typeOf(&v);
    if (!type)
        return false;
    Visual r;
    const std::string &k = type->s;
    const Json *size = g->get("size");
    if (k == "box") {
        r.kind = Visual::Box;
        V s;
        if (vec3(g->get("half_size"), s) || vec3(g->get("half_extents"), s))
            r.half = s;
        else if (vec3(size, s))
            r.half = s * .5;
        else
            return false;
    } else if (k == "sphere") {
        r.kind = Visual::Sphere;
        r.radius = num(g->get("radius"), size && size->numbers() ? size->a[0].n : -1);
    } else if (k == "cylinder" || k == "capsule") {
        r.kind = k == "cylinder" ? Visual::Cylinder : Visual::Capsule;
        const Json *rad = g->get("radius"), *len = g->get("length");
        if (rad && len) {
            r.radius = num(rad, -1);
            r.halfLength = num(len, -1) * .5;
        } else if (size && size->numbers() && size->a.size() >= 2) {
            r.radius = size->a[0].n;
            r.halfLength = size->a[1].n;
        } else
            return false;
    } else
        return false; // mesh, plane, ... (primitives only in v1)
    if (!(r.radius >= 0) || !(r.halfLength >= 0) || !std::isfinite(r.half.x + r.half.y + r.half.z))
        return false;
    if ((r.kind != Visual::Box) && r.radius <= 0)
        return false;
    // The visual's own pose: origin/pose/pos+quat on the visual, else on the geometry.
    if (!parsePose(&v, r.local) && g != &v)
        parsePose(g, r.local);
    const Json *color = nullptr;
    for (const Json *src : {&v, g, v.get("material")})
        if (src && src->isObj())
            for (const char *c : {"rgba", "color", "colour"})
                if (!color) {
                    const Json *x = src->get(c);
                    if (x && x->numbers() && x->a.size() >= 3)
                        color = x;
                }
    if (color) {
        r.rgb = {color->a[0].n, color->a[1].n, color->a[2].n};
        r.coloured = true;
    }
    out = r;
    return true;
}
// A single visual object or an array of them, appended. Returns how many were skipped.
inline int parseVisuals(const Json *j, std::vector<Visual> &out) {
    int skipped = 0;
    if (!j)
        return 0;
    auto one = [&](const Json &v) {
        Visual x;
        if (parseVisual(v, x))
            out.push_back(x);
        else
            skipped++;
    };
    if (j->isArr())
        for (const auto &v : j->a)
            one(v);
    else if (j->isObj())
        one(*j);
    return skipped;
}

// ---------------------------------------------------------------- model -----
struct Joint {
    std::string name, type, parent, child;
    Pose origin;
    V axis{0, 0, 1};
    std::string mimic;
    double multiplier = 1, offset = 0;
    bool movable() const { return type == "revolute" || type == "continuous" || type == "prismatic"; }
};
struct Model {
    std::string version, root;
    std::map<std::string, std::vector<Visual>> visuals;
    std::vector<Joint> joints; // Ordered parents-first from the root; unreachable joints dropped.
    int skippedVisuals = 0;
};
inline bool parseModel(const Json &j, Model &out, std::string *error = nullptr) {
    auto fail = [&](const char *why) {
        if (error)
            *error = why;
        return false;
    };
    if (!j.isObj())
        return fail("model is not an object");
    Model m;
    if (const Json *v = j.get("model_version"))
        m.version = v->isStr() ? v->s : v->isNum() ? std::to_string(int64_t(v->n)) : "";
    // Links: {name: {visuals}} or [{name, visuals}].
    const Json *links = j.get("links");
    auto addLink = [&](const std::string &name, const Json &body) {
        auto &list = m.visuals[name];
        const Json *v = body.get("visuals");
        if (!v)
            v = body.get("visual");
        if (!v)
            v = body.get("geoms");
        m.skippedVisuals += parseVisuals(v, list);
    };
    if (links && links->isObj())
        for (const auto &kv : links->o)
            addLink(kv.first, kv.second);
    else if (links && links->isArr())
        for (const auto &l : links->a)
            if (const Json *n = l.get("name"))
                if (n->isStr())
                    addLink(n->s, l);
    // Joints: [{name,type,parent,child,origin,axis,mimic}] (or {name: {...}}).
    const Json *joints = j.get("joints");
    const Json *frames = j.get("frames"); // GET /model's frame tree carries origins per child.
    std::vector<Joint> all;
    auto addJoint = [&](const std::string &fallbackName, const Json &b) {
        if (!b.isObj())
            return;
        Joint x;
        const Json *s;
        x.name = (s = b.get("name")) && s->isStr() ? s->s : fallbackName;
        x.type = (s = b.get("type")) && s->isStr() ? s->s : "fixed";
        if ((s = b.get("parent")) && s->isStr())
            x.parent = s->s;
        if ((s = b.get("child")) && s->isStr())
            x.child = s->s;
        if (x.parent.empty() || x.child.empty())
            return;
        bool posed = parsePose(&b, x.origin);
        if (!posed && frames && frames->isObj())
            if (const Json *f = frames->get(x.child))
                parsePose(f, x.origin);
        V axis;
        if (vec3(b.get("axis"), axis) && teleop::norm(axis) > 1e-9)
            x.axis = teleop::unit(axis);
        if (const Json *mm = b.get("mimic")) {
            if (mm->isStr())
                x.mimic = mm->s;
            else if (mm->isObj()) {
                if ((s = mm->get("joint")) && s->isStr())
                    x.mimic = s->s;
                x.multiplier = num(mm->get("multiplier"), 1);
                x.offset = num(mm->get("offset"), 0);
            }
        }
        all.push_back(x);
    };
    if (joints && joints->isArr())
        for (const auto &b : joints->a)
            addJoint("", b);
    else if (joints && joints->isObj())
        for (const auto &kv : joints->o)
            addJoint(kv.first, kv.second);
    // Root: explicit, else a link that is nobody's child.
    for (const char *k : {"root", "base_link"})
        if (const Json *r = j.get(k))
            if (r->isStr() && !r->s.empty()) {
                m.root = r->s;
                break;
            }
    if (m.root.empty()) {
        std::map<std::string, bool> isChild;
        for (const auto &x : all)
            isChild[x.child] = true;
        for (const auto &x : all)
            if (!isChild[x.parent]) {
                m.root = x.parent;
                break;
            }
        if (m.root.empty() && !m.visuals.empty())
            m.root = m.visuals.begin()->first;
    }
    if (m.root.empty())
        return fail("model has no root link");
    // Parents-first order by breadth-first search from the root.
    std::vector<std::string> frontier{m.root};
    std::map<std::string, bool> seen{{m.root, true}};
    while (!frontier.empty()) {
        std::vector<std::string> next;
        for (const auto &p : frontier)
            for (const auto &x : all)
                if (x.parent == p && !seen[x.child]) {
                    seen[x.child] = true;
                    m.joints.push_back(x);
                    next.push_back(x.child);
                }
        frontier.swap(next);
    }
    out = std::move(m);
    return true;
}

// ---------------------------------------------------------------- state -----
struct Marker {
    enum Role { Frame, Target, Command, Arrow } role = Frame;
    std::string name;
    Pose pose;         // World (sim) frame, or base frame when inBase.
    V arrow;           // Arrow: base-frame [vx, vy, wz].
    bool inBase = false;
    bool limited = false;
};
struct Object {
    std::string name;
    Pose pose;
    std::vector<Visual> geoms;
};
struct State {
    double t = 0;
    int64_t seq = -1;
    std::string modelVersion;
    bool hasResetSeq = false; // reset_seq: a change re-anchors the AR scene.
    int64_t resetSeq = 0;
    Pose base;
    bool hasBase = false;
    std::map<std::string, double> joints;
    std::vector<Marker> markers;
    bool limited = false;
    std::string reason, baseMode;
    bool hasObjects = false;
    std::vector<Object> objects;
};
inline bool contains(const std::string &s, const char *w) { return s.find(w) != std::string::npos; }
inline std::string lower(std::string s) {
    for (auto &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
// Overlays are free-form; recognise poses (frames), base-relative velocity hints
// (arrows), "limited" flags, reasons and base modes, recursing into objects.
inline void walkOverlays(const Json &j, const std::string &path, State &st, int depth = 0) {
    if (!j.isObj() || depth > 3)
        return;
    for (const auto &kv : j.o) {
        const std::string key = lower(kv.first), name = path.empty() ? kv.first : path + "/" + kv.first;
        const Json &v = kv.second;
        if (key == "limited" && v.type == Json::Bool) {
            st.limited = st.limited || v.b;
            continue;
        }
        if ((key == "reason" || key == "limited_reason" || key == "limit_reason") && v.isStr()) {
            if (st.reason.empty())
                st.reason = v.s;
            continue;
        }
        if (contains(key, "mode") && v.isStr()) {
            if (st.baseMode.empty())
                st.baseMode = v.s;
            continue;
        }
        bool hintKey = contains(key, "hint") || contains(key, "velocity") || contains(key, "twist");
        if (hintKey && v.numbers() && (v.a.size() == 2 || v.a.size() == 3)) {
            Marker mk;
            mk.role = Marker::Arrow;
            mk.name = name;
            mk.arrow = {v.a[0].n, v.a[1].n, v.a.size() == 3 ? v.a[2].n : 0};
            st.markers.push_back(mk);
            continue;
        }
        Pose p;
        bool posed = v.numbers(7) || (v.isObj() && v.get("pose") && parsePose(v.get("pose"), p));
        if (v.numbers(7))
            parsePose(&v, p);
        if (posed) {
            Marker mk;
            mk.name = name;
            mk.pose = p;
            const std::string full = lower(name);
            mk.role = contains(full, "target")                              ? Marker::Target
                      : contains(full, "command") || contains(full, "cmd") ? Marker::Command
                                                                             : Marker::Frame;
            if (v.isObj()) {
                const Json *f = v.get("frame");
                mk.inBase = f && f->isStr() && contains(lower(f->s), "base");
                const Json *l = v.get("limited");
                mk.limited = l && l->type == Json::Bool && l->b;
                st.limited = st.limited || mk.limited;
                const Json *r = v.get("reason");
                if (mk.limited && r && r->isStr() && st.reason.empty())
                    st.reason = r->s;
            }
            st.markers.push_back(mk);
            continue;
        }
        if (v.isObj()) {
            if (hintKey) {
                V vel;
                if (vec3(v.get("velocity"), vel) || vec3(v.get("twist"), vel)) {
                    Marker mk;
                    mk.role = Marker::Arrow;
                    mk.name = name;
                    mk.arrow = vel;
                    st.markers.push_back(mk);
                }
            }
            walkOverlays(v, name, st, depth + 1);
        }
    }
}
inline bool parseObjects(const Json *j, std::vector<Object> &out) {
    if (!j || !j->isObj())
        return false;
    for (const auto &kv : j->o) {
        Object ob;
        ob.name = kv.first;
        if (!parsePose(kv.second.get("pose"), ob.pose))
            continue;
        const Json *g = kv.second.get("geom");
        if (!g)
            g = kv.second.get("geoms");
        if (!g)
            g = kv.second.get("visuals");
        parseVisuals(g, ob.geoms);
        out.push_back(ob);
    }
    return true;
}
inline bool parseState(const Json &j, State &out) {
    if (!j.isObj())
        return false;
    State s;
    s.t = num(j.get("t"), 0);
    s.seq = int64_t(num(j.get("seq"), -1));
    if (const Json *v = j.get("model_version"))
        s.modelVersion = v->isStr() ? v->s : v->isNum() ? std::to_string(int64_t(v->n)) : "";
    if (const Json *r = j.get("reset_seq"))
        if (r->isNum() && std::isfinite(r->n)) {
            s.hasResetSeq = true;
            s.resetSeq = int64_t(r->n);
        }
    s.hasBase = parsePose(j.get("base"), s.base);
    if (const Json *q = j.get("joints"))
        if (q->isObj())
            for (const auto &kv : q->o)
                if (kv.second.isNum() && std::isfinite(kv.second.n))
                    s.joints[kv.first] = kv.second.n;
    if (const Json *o = j.get("overlays"))
        walkOverlays(*o, "", s);
    s.hasObjects = parseObjects(j.get("objects"), s.objects);
    if (!s.hasBase && s.joints.empty() && !s.hasObjects)
        return false;
    out = std::move(s);
    return true;
}

// ---------------------------------------------------------- kinematics ------
// Every reachable link's pose in the sim world. Unknown joints read 0; mimic
// joints follow their leader. The root link sits at the state's base pose.
inline std::map<std::string, Pose> forwardKinematics(const Model &m, const State &s) {
    std::map<std::string, Pose> out;
    out[m.root] = s.hasBase ? s.base : Pose{};
    auto position = [&](const Joint &x) {
        auto it = s.joints.find(x.mimic.empty() ? x.name : x.mimic);
        double q = it == s.joints.end() ? 0 : it->second;
        return x.mimic.empty() ? q : x.multiplier * q + x.offset;
    };
    for (const auto &x : m.joints) {
        auto parent = out.find(x.parent);
        if (parent == out.end())
            continue;
        Pose motion;
        if (x.type == "revolute" || x.type == "continuous")
            motion.r = teleop::rotation(x.axis, position(x));
        else if (x.type == "prismatic")
            motion.p = x.axis * position(x);
        out[x.child] = teleop::compose(teleop::compose(parent->second, x.origin), motion);
    }
    return out;
}

// ----------------------------------------------------------- placement ------
// Where the sim world sits in the operator space. The origin is 1.6 m ahead of
// the head along a heading, at the floor height: the runtime's floor space
// (re-read for every placement), or 1.6 m below the head when none is located
// (floorEstimated); +x is the heading. Offset (metres, in the placed frame), yaw (rad about up) and uniform
// scale from the config apply on top.
// Every placement uses the head position and the head's gravity-aligned yaw at
// that moment (the operator forward / A realignment never feeds it):
//  - the first tracked head, and a provider-session (LOCAL reference-space) change;
//  - a reset: a change of the state's reset_seq (the first value seen is only a
//    baseline), or, when the state carries no reset_seq, an A press (focused,
//    head tracked). Y does nothing here.
struct Placement {
    bool placed = false;
    int64_t session = 0;
    Pose origin;
    V offset;
    double yaw = 0, scale = 1;
    bool seqSeen = false, pending = false;
    int64_t lastSeq = 0;
    bool floorEstimated = false; // The last placement used head - 1.6 m (no floor space).
    // floor: the runtime's floor height (FLU z) when located, else null.
    void reset(V head, const M &heading, const double *floor = nullptr) {
        V ahead = head + heading * V{1.6, 0, 0};
        origin = {{ahead.x, ahead.y, teleop::floorHeight(head, floor)}, heading};
        floorEstimated = !floor;
        placed = true;
    }
    // Gravity-aligned yaw of the headset (head pose in LOCAL-FLU). Looking straight
    // down or up, the headset's up (or down) vector gives the facing instead.
    static M headYaw(const Pose &head) {
        // operatorPose keeps the headset's body axes: view = its -Z, up = its +Y.
        V f = head.r * V{0, 0, -1};
        if (std::hypot(f.x, f.y) < .15)
            f = head.r * V{0, 1, 0} * (f.z < 0 ? 1. : -1.);
        return teleop::rotation({0, 0, 1}, std::atan2(f.y, f.x));
    }
    // headFlu: head pose in LOCAL-FLU (teleop::operatorPose of the OpenXR pose).
    // resetSeq: the newest state's reset_seq, null when absent or no state yet.
    // aPress: A rising edge while input is focused. True when it re-anchored.
    // floor: the floor height re-read this frame (null when no floor space is located).
    bool update(int64_t providerSession, const Pose &headFlu, bool headValid, const int64_t *resetSeq, bool aPress,
                const double *floor = nullptr) {
        if (!placed || session != providerSession) {
            placed = false;
            if (headValid)
                reset(headFlu.p, headYaw(headFlu), floor);
            session = providerSession;
        }
        if (resetSeq) {
            if (!seqSeen) {
                seqSeen = true;
                lastSeq = *resetSeq;
            } else if (*resetSeq != lastSeq) {
                lastSeq = *resetSeq;
                pending = true; // Applied as soon as the head is tracked.
            }
        } else if (aPress && headValid)
            pending = true;
        if (!pending || !headValid)
            return false;
        pending = false;
        reset(headFlu.p, headYaw(headFlu), floor);
        return true;
    }
    Pose world() const { return teleop::compose(origin, {offset, teleop::rotation({0, 0, 1}, yaw)}); }
    // Sim-world pose -> operator FLU (position scaled about the placed origin).
    Pose map(const Pose &p) const {
        Pose w = world();
        return {w.p + w.r * (p.p * scale), w.r * p.r};
    }
};

// -------------------------------------------------------------- config ------
struct Config {
    bool enabled = false;
    std::string modelUrl, stateUrl, objectsUrl;
    V offset;
    double yawDeg = 0, scale = 1;
};
inline std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
// key=value lines (written by `quest run --ar=...`): url, model, state, objects,
// offset=x,y,z, yaw=deg, scale=s. `url` is the robot's frontend root: the model
// and state default to its description node and its sim node's state stream.
inline Config parseConfig(const std::string &text) {
    Config c;
    std::string base;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        std::string line = trim(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        start = end == std::string::npos ? text.size() + 1 : end + 1;
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos)
            continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "url")
            base = v;
        else if (k == "model")
            c.modelUrl = v;
        else if (k == "state")
            c.stateUrl = v;
        else if (k == "objects")
            c.objectsUrl = v;
        else if (k == "yaw")
            c.yawDeg = std::strtod(v.c_str(), nullptr);
        else if (k == "scale") {
            double s = std::strtod(v.c_str(), nullptr);
            c.scale = s > 1e-3 && s < 1e3 ? s : 1;
        } else if (k == "offset") {
            double x[3] = {};
            const char *p = v.c_str();
            for (int i = 0; i < 3 && *p; i++) {
                char *e = nullptr;
                x[i] = std::strtod(p, &e);
                if (e == p)
                    break;
                p = *e == ',' ? e + 1 : e;
            }
            c.offset = {x[0], x[1], x[2]};
        }
    }
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    if (!base.empty()) {
        if (c.modelUrl.empty())
            c.modelUrl = base + "/nodes/description/model/tree?variant=boxes";
        if (c.stateUrl.empty())
            c.stateUrl = base + "/nodes/sim/robot_state/stream?format=jsonl&hz=30";
    }
    if (!std::isfinite(c.yawDeg))
        c.yawDeg = 0;
    if (!std::isfinite(c.offset.x + c.offset.y + c.offset.z))
        c.offset = {};
    c.enabled = !c.stateUrl.empty();
    return c;
}

// ------------------------------------------------------------ rendering -----
constexpr double kStaleAfterS = 0.5; // Receipt age beyond which the pose is drawn dimmed.
constexpr double kStaleDim = .3;
inline V shade(V c) {
    // Keep very dark sim colours visible over passthrough.
    double m = std::max(c.x, std::max(c.y, c.z));
    return m < .3 ? V{.5, .5, .5} : c;
}
inline void drawVisual(teleop::Wire &w, const Placement &pl, const Pose &frame, const Visual &v, V color) {
    Pose p = pl.map(teleop::compose(frame, v.local));
    double s = pl.scale;
    switch (v.kind) {
    case Visual::Box: w.box(p, v.half * s, color); break;
    case Visual::Sphere: w.sphere(p.p, v.radius * s, color); break;
    case Visual::Cylinder:
        w.link(p.p - p.r * V{0, 0, v.halfLength * s}, p.p + p.r * V{0, 0, v.halfLength * s}, v.radius * s, color);
        break;
    case Visual::Capsule:
        w.capsule(p.p - p.r * V{0, 0, v.halfLength * s}, p.p + p.r * V{0, 0, v.halfLength * s}, v.radius * s,
                  color);
        break;
    }
}
// The whole AR scene in operator FLU. `model` may be null (state only: base axes).
// `extraObjects` (a separate objects stream) is drawn only when the state lines carry none.
inline void buildScene(teleop::Wire &w, const Model *model, const State *state, const Placement &pl, bool stale,
                       const std::vector<Object> *extraObjects = nullptr) {
    if (!pl.placed)
        return;
    const size_t first = w.segs.size();
    struct Dim { // Stale: the last pose stays, every colour (axes included) dimmed.
        teleop::Wire &w;
        size_t first;
        bool on;
        ~Dim() {
            if (on)
                for (size_t i = first; i < w.segs.size(); i++)
                    w.segs[i].color = w.segs[i].color * kStaleDim;
        }
    } dim{w, first, stale};
    // A small origin cross on the floor marks where the sim world is anchored.
    V faint = shade({.35, .55, .5});
    Pose o = pl.world();
    for (V d : {V{.15, 0, 0}, V{0, .15, 0}})
        w.line(o.p - o.r * d, o.p + o.r * d, faint);
    if (!state)
        return;
    const Pose base = state->hasBase ? state->base : Pose{};
    if (model) {
        auto links = forwardKinematics(*model, *state);
        for (const auto &kv : model->visuals) {
            auto it = links.find(kv.first);
            if (it == links.end())
                continue;
            for (const auto &v : kv.second)
                drawVisual(w, pl, it->second, v, shade(v.rgb));
        }
    } else {
        Pose b = pl.map(base);
        w.axes(b, .25 * pl.scale);
    }
    const auto &objects = state->hasObjects || !extraObjects ? state->objects : *extraObjects;
    for (const auto &ob : objects)
        for (const auto &v : ob.geoms)
            drawVisual(w, pl, ob.pose, v, shade(v.coloured ? v.rgb : V{.85, .8, .55}));
    const V red{1, .15, .15}, ok{.6, 1, .5};
    for (const auto &mk : state->markers) {
        if (mk.role == Marker::Arrow) {
            // Base-frame planar velocity hint, drawn 1 s ahead (capped), 5 cm above the floor.
            V d = base.r * V{mk.arrow.x, mk.arrow.y, 0};
            d.z = 0;
            double l = teleop::norm(d);
            if (l > .6)
                d = d * (.6 / l);
            Pose start = pl.map({base.p + V{0, 0, .05}, {}});
            Pose end = pl.map({base.p + V{0, 0, .05} + d, {}});
            w.arrow(start.p, end.p - start.p, shade({.9, .9, .9}));
            continue;
        }
        Pose p = pl.map(mk.inBase ? teleop::compose(base, mk.pose) : mk.pose);
        bool lim = mk.limited || (state->limited && mk.role != Marker::Frame);
        double s = pl.scale;
        if (mk.role == Marker::Target) {
            w.axes(p, .065 * s);
            w.box(p, V{.015, .015, .015} * s, shade(lim ? red : ok));
        } else if (mk.role == Marker::Command) {
            w.axes(p, .10 * s);
            if (lim)
                w.box(p, V{.022, .022, .022} * s, shade(red));
        } else
            w.axes(p, .08 * s);
    }
    if (state->limited) {
        // Colour cue: a red ring on the floor around the base.
        Pose b = pl.map({base.p + V{0, 0, .02}, base.r});
        w.arc(b.p, b.r * V{1, 0, 0}, b.r * V{0, 1, 0}, .55 * pl.scale, 0, 2 * teleop::pi, shade(red), 24);
    }
}

// ---------------------------------------------------------------- HUD -------
// HUD font: A-Z, 0-9, '-', '.', '/', space.
inline std::string hudText(const std::string &s, size_t max = 34) {
    std::string out;
    for (char c : s) {
        char u = char(std::toupper(static_cast<unsigned char>(c)));
        bool ok = (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '-' || u == '.' || u == '/';
        out += ok ? u : ' ';
        if (out.size() >= max)
            break;
    }
    return out;
}
} // namespace robot_ar
