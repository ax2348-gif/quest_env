// Host tests for the robot AR view: JSON/description/state parsing, FK,
// placement, staleness, HTTP framing and (with --client URL) the background
// client against tools/fake_robot_server.py.
#include "robot_ar_client.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace robot_ar;
using teleop::compose;
using teleop::norm;
static bool near(double a, double b, double e = 1e-9) { return std::abs(a - b) <= e; }
static bool near(V a, V b, double e = 1e-9) { return norm(a - b) <= e; }
static bool near(const M &a, const M &b, double e = 1e-9) {
    for (int i = 0; i < 9; i++)
        if (!near(a.v[i], b.v[i], e))
            return false;
    return true;
}
static Json parse(const std::string &s) {
    Json j;
    bool ok = parseJson(s, j);
    if (!ok)
        fprintf(stderr, "bad JSON: %s\n", s.c_str());
    assert(ok);
    return j;
}

static void testJson() {
    Json j = parse(R"( {"a": [1, -2.5e1, true, false, null], "s": "x\"\\\né", "o": {}, "e": []} )");
    assert(j.get("a")->a.size() == 5 && near(j.get("a")->a[1].n, -25) && j.get("a")->a[2].b);
    assert(j.get("s")->s == "x\"\\\n\xc3\xa9");
    assert(j.get("o")->isObj() && j.get("e")->isArr());
    assert(parse("NaN").type == Json::Null);
    Json bad;
    for (const char *s : {"{", "[1,]", "{\"a\" 1}", "tru", "\"open", "{} x", ""})
        assert(!parseJson(s, bad));
    std::string deep(200, '[');
    assert(!parseJson(deep + std::string(200, ']'), bad));
    puts("PASS json");
}

static const char *kModel = R"({
  "model_version": "v7", "root": "base",
  "links": [
    {"name": "base", "visuals": [{"type": "box", "size": [0.6, 0.4, 0.2], "origin": {"pos": [0, 0, 0.1], "quat": [0, 0, 0, 1]},
                                  "rgba": [0.2, 0.4, 0.6, 1]}, {"type": "mesh", "filename": "x.stl"}]},
    {"name": "upper", "visuals": [{"type": "capsule", "radius": 0.03, "length": 0.3, "origin": {"pos": [0.15, 0, 0], "rpy": [0, 1.5707963267948966, 0]}}]},
    {"name": "lower", "visuals": [{"geometry": {"type": "cylinder", "size": [0.02, 0.1]}, "material": {"color": [1, 0, 0, 1]}}]},
    {"name": "tip", "visuals": {"type": "sphere", "radius": 0.01}},
    {"name": "finger_a"}, {"name": "finger_b"}, {"name": "orphan", "visuals": [{"type": "box", "half_size": [1, 1, 1]}]}
  ],
  "joints": [
    {"name": "finger_b", "type": "prismatic", "parent": "lower", "child": "finger_b", "axis": [0, 1, 0],
     "mimic": {"joint": "finger_a", "multiplier": -1, "offset": 0.01}},
    {"name": "elbow", "type": "revolute", "parent": "upper", "child": "lower",
     "origin": {"pos": [0.3, 0, 0], "quat": [0, 0, 0, 1]}, "axis": [0, 0, 2]},
    {"name": "shoulder", "type": "revolute", "parent": "base", "child": "upper",
     "origin": {"xyz": [0, 0, 1], "rpy": [0, 0, 0]}, "axis": [0, 0, 1], "lower": -2, "upper": 2},
    {"name": "wrist_fixed", "type": "fixed", "parent": "lower", "child": "tip", "origin": [0.2, 0, 0, 0, 0, 0, 1]},
    {"name": "finger_a", "type": "prismatic", "parent": "lower", "child": "finger_a", "axis": [0, 1, 0]},
    {"name": "dangling", "type": "fixed", "parent": "nowhere", "child": "orphan"}
  ]})";

static void testModel() {
    Model m;
    std::string why;
    assert(parseModel(parse(kModel), m, &why));
    assert(m.version == "v7" && m.root == "base");
    assert(m.skippedVisuals == 1); // The mesh, primitives only in v1.
    assert(m.visuals["base"].size() == 1);
    const Visual &box = m.visuals["base"][0];
    assert(box.kind == Visual::Box && near(box.half, V{.3, .2, .1}) && box.coloured && near(box.rgb, V{.2, .4, .6}));
    const Visual &cap = m.visuals["upper"][0];
    assert(cap.kind == Visual::Capsule && near(cap.radius, .03) && near(cap.halfLength, .15));
    assert(near(cap.local.r * V{0, 0, 1}, V{1, 0, 0}, 1e-9)); // rpy pitch +90 deg turns z onto x.
    const Visual &cyl = m.visuals["lower"][0];
    assert(cyl.kind == Visual::Cylinder && near(cyl.radius, .02) && near(cyl.halfLength, .1)); // MuJoCo size.
    assert(cyl.coloured && near(cyl.rgb, V{1, 0, 0}));
    assert(m.visuals["tip"].size() == 1 && m.visuals["tip"][0].kind == Visual::Sphere);
    assert(near(m.visuals["orphan"][0].half, V{1, 1, 1}));
    // Parents first; the unreachable joint is dropped.
    assert(m.joints.size() == 5);
    std::map<std::string, size_t> at;
    for (size_t i = 0; i < m.joints.size(); i++)
        at[m.joints[i].name] = i;
    assert(!at.count("dangling"));
    assert(at["shoulder"] < at["elbow"] && at["elbow"] < at["finger_b"] && at["elbow"] < at["wrist_fixed"]);
    assert(near(m.joints[at["elbow"]].axis, V{0, 0, 1})); // Normalised.
    // Root inferred when absent.
    Model inferred;
    assert(parseModel(parse(R"({"joints": [{"name": "j", "type": "fixed", "parent": "a", "child": "b"}]})"),
                      inferred));
    assert(inferred.root == "a");
    assert(!parseModel(parse("{}"), inferred, &why));
    // GET /model style: origins from the frame tree (xyz + rpy per child).
    Model framed;
    assert(parseModel(parse(R"({"root": "a", "joints": [{"name": "j", "type": "revolute", "parent": "a", "child": "b", "axis": [0, 0, 1]}],
                                "frames": {"b": {"parent": "a", "xyz": [1, 2, 3], "rpy": [0, 0, 0.5]}}})"),
                      framed));
    assert(near(framed.joints[0].origin.p, V{1, 2, 3}) &&
               near(framed.joints[0].origin.r, teleop::rotation({0, 0, 1}, .5)));
    puts("PASS description parsing (links, visual conventions, joint order, mimic, root, frame-tree origins)");
}

static void testStateAndFk() {
    Model m;
    assert(parseModel(parse(kModel), m));
    State s;
    const double yaw = teleop::pi / 2;
    std::string line = R"({"t": 1.5, "seq": 42, "model_version": "v7",
        "base": [1, 2, 0, 0, 0, )" + std::to_string(std::sin(yaw / 2)) + "," + std::to_string(std::cos(yaw / 2)) +
                       R"(],
        "joints": {"shoulder": 1.5707963267948966, "elbow": 0, "finger_a": 0.02, "unknown": 3, "bad": null},
        "overlays": {"wbc": {"left_target": {"pose": [0, 0, 1, 0, 0, 0, 1], "limited": true, "reason": "reach"},
                             "right_command": [1, 1, 1, 0, 0, 0, 1], "base_hint": [0.2, 0, 0.1], "base_mode": "auto",
                             "nested": {"frame_a": {"pose": [0, 0, 0.5, 0, 0, 0, 1], "frame": "base"}}}},
        "objects": {"cube": {"pose": [3, 0, 0.5, 0, 0, 0, 1], "geom": {"type": "box", "size": [0.1, 0.1, 0.1]}}}})";
    assert(parseState(parse(line), s));
    assert(s.seq == 42 && near(s.t, 1.5) && s.modelVersion == "v7" && s.hasBase);
    assert(s.joints.size() == 4 && !s.joints.count("bad"));
    assert(s.limited && s.reason == "reach" && s.baseMode == "auto");
    assert(s.markers.size() == 4);
    int targets = 0, commands = 0, arrows = 0, frames = 0;
    for (const auto &mk : s.markers) {
        targets += mk.role == Marker::Target;
        commands += mk.role == Marker::Command;
        arrows += mk.role == Marker::Arrow;
        frames += mk.role == Marker::Frame;
        if (mk.role == Marker::Target)
            assert(mk.limited && !mk.inBase);
        if (mk.role == Marker::Frame)
            assert(mk.inBase && mk.name == "wbc/nested/frame_a");
        if (mk.role == Marker::Arrow)
            assert(near(mk.arrow, V{.2, 0, .1}));
    }
    assert(targets == 1 && commands == 1 && arrows == 1 && frames == 1);
    assert(s.hasObjects && s.objects.size() == 1 && s.objects[0].geoms.size() == 1);

    auto links = forwardKinematics(m, s);
    assert(!links.count("orphan"));
    // base at (1,2,0) yawed +90: base x -> world y.
    assert(near(links["base"].p, V{1, 2, 0}, 1e-6));
    // Shoulder 1 m up, then +90 deg about z: upper's x points along world -x.
    assert(near(links["upper"].p, V{1, 2, 1}, 1e-6));
    assert(near(links["upper"].r * V{1, 0, 0}, V{-1, 0, 0}, 1e-6));
    // Elbow 0.3 along upper x.
    assert(near(links["lower"].p, V{.7, 2, 1}, 1e-6));
    assert(near(links["tip"].p, V{.5, 2, 1}, 1e-6));
    // Prismatic along lower y (= world -y); mimic: -1 * 0.02 + 0.01 = -0.01.
    assert(near(links["finger_a"].p, V{.7, 2 - .02, 1}, 1e-6));
    assert(near(links["finger_b"].p, V{.7, 2 + .01, 1}, 1e-6));
    // Missing joints read zero; missing base = identity.
    State empty;
    assert(parseState(parse(R"({"joints": {}, "base": [0,0,0,0,0,0,1]})"), empty));
    auto zero = forwardKinematics(m, empty);
    assert(near(zero["lower"].p, V{.3, 0, 1}));
    assert(!parseState(parse(R"({"t": 1})"), empty)); // Neither base, joints nor objects.
    puts("PASS robot_state.v1 parsing (overlays, objects) and FK (revolute, prismatic, fixed, mimic, base)");
}

static teleop::Pose fluHead(V questPos, const M &questRot) { return teleop::operatorPose({questPos, questRot}); }

static void testPlacement() {
    using teleop::rotation;
    const double pi = teleop::pi;
    // Every placement = head position + the head's gravity-aligned yaw at that moment;
    // Placement::update takes no operator forward, so A's realignment cannot feed it.
    // Quest head at LOCAL (0.1, 1.7, -0.2) looking along LOCAL -X (operator left), pitched down.
    M lookLeft = rotation({0, 1, 0}, pi / 2) * rotation({1, 0, 0}, -.3);
    teleop::Pose head = fluHead({.1f, 1.7f, -.2f}, lookLeft);
    assert(near(head.p, V{.2, -.1, 1.7}, 1e-6));
    const V leftOrigin{.2, 1.5, .1}, aheadOrigin{1.6, 0, 0};
    const M leftYaw = rotation({0, 0, 1}, pi / 2);
    teleop::Pose ahead = fluHead({0, 1.6f, 0}, M{});
    // First tracked head: placed at the head yaw (1.6 m to the operator's left here).
    Placement pl;
    assert(!pl.update(7, head, false, nullptr, false) && !pl.placed);
    assert(!pl.update(7, head, true, nullptr, false));
    assert(pl.placed && near(pl.origin.p, leftOrigin, 1e-6) && near(pl.origin.r, leftYaw, 1e-9));
    // Nothing pressed, no reset_seq: stays put whatever the head does (Y is not an input).
    for (int i = 0; i < 3; i++)
        assert(!pl.update(7, ahead, true, nullptr, false));
    assert(near(pl.origin.p, leftOrigin, 1e-6));
    // Without reset_seq, A re-anchors at the head yaw.
    assert(pl.update(7, ahead, true, nullptr, true));
    assert(near(pl.origin.r, M{}, 1e-9) && near(pl.origin.p, aheadOrigin, 1e-6));
    // A while the head is untracked is ignored, not deferred.
    assert(!pl.update(7, head, false, nullptr, true));
    assert(!pl.update(7, head, true, nullptr, false));
    assert(near(pl.origin.p, aheadOrigin, 1e-6));
    // The first reset_seq is a baseline only; unchanged values do nothing; A is ignored with reset_seq.
    int64_t seq = 5;
    assert(!pl.update(7, head, true, &seq, false));
    assert(!pl.update(7, head, true, &seq, true));
    assert(near(pl.origin.p, aheadOrigin, 1e-6));
    // A changed reset_seq re-anchors at the head yaw.
    seq = 6;
    assert(pl.update(7, head, true, &seq, false));
    assert(near(pl.origin.r, leftYaw, 1e-9) && near(pl.origin.p, leftOrigin, 1e-6));
    // A change while the head is untracked is applied once tracking returns.
    seq = 7;
    assert(!pl.update(7, ahead, false, &seq, false));
    assert(near(pl.origin.p, leftOrigin, 1e-6));
    assert(pl.update(7, ahead, true, &seq, false));
    assert(near(pl.origin.p, aheadOrigin, 1e-6));
    // A missing reset_seq (stream gap) neither re-anchors nor resets the baseline.
    assert(!pl.update(7, head, true, nullptr, false));
    assert(!pl.update(7, head, true, &seq, false));
    assert(near(pl.origin.p, aheadOrigin, 1e-6));
    // A LOCAL reference-space change (provider session) re-places at the head yaw too.
    assert(!pl.update(8, head, false, &seq, false));
    assert(!pl.placed);
    assert(!pl.update(8, head, true, &seq, false));
    assert(pl.placed && near(pl.origin.p, leftOrigin, 1e-6) && near(pl.origin.r, leftYaw, 1e-9));
    assert(!pl.update(8, ahead, true, &seq, false)); // Baseline kept across the session change.
    // Looking straight down or up, the headset's up/down vector gives the yaw.
    M down = rotation({0, 1, 0}, pi / 2) * rotation({1, 0, 0}, -pi / 2); // Facing LOCAL -X, nose down.
    assert(near(Placement::headYaw(fluHead({}, down)), rotation({0, 0, 1}, pi / 2), 1e-9));
    M up = rotation({0, 1, 0}, pi / 2) * rotation({1, 0, 0}, pi / 2);
    assert(near(Placement::headYaw(fluHead({}, up)), rotation({0, 0, 1}, pi / 2), 1e-9));
    // reset_seq is parsed when present, absent otherwise.
    State st;
    assert(parseState(parse(R"({"base": [0,0,0,0,0,0,1], "reset_seq": 3})"), st) && st.hasResetSeq &&
           st.resetSeq == 3);
    assert(parseState(parse(R"({"base": [0,0,0,0,0,0,1]})"), st) && !st.hasResetSeq);
    // Sim world point -> operator: offset, yaw, scale.
    Placement q;
    q.reset({0, 0, 1.6}, M{});
    assert(near(q.map({{1, 0, 0}, {}}).p, V{2.6, 0, 0}));
    q.offset = {0, .5, 0};
    q.yaw = pi / 2;
    q.scale = .5;
    assert(near(q.map({{1, 0, 0}, {}}).p, V{1.6, 1, 0}, 1e-9));
    assert(near(q.map({{0, 0, 2}, {}}).p, V{1.6, .5, 1}, 1e-9));
    puts("PASS placement (head position + yaw for the first head, session change and resets; reset_seq "
         "baseline/change, A only without reset_seq, no Y, untracked head, offset/yaw/scale)");
}

static void testConfig() {
    Config c = parseConfig("url=http://erid:8080/\n");
    assert(c.enabled && c.modelUrl == "http://erid:8080/nodes/description/model/tree?variant=simple");
    assert(c.stateUrl == "http://erid:8080/nodes/sim/robot_state/stream?format=jsonl&hz=30");
    c = parseConfig("# x\nurl=http://a\nstate = http://robot:8200/robot_state/stream\nobjects=http://o/s\n"
                    "offset=0.5,-1,0.25\nyaw=90\nscale=0.5\n");
    assert(c.stateUrl == "http://robot:8200/robot_state/stream" && c.objectsUrl == "http://o/s");
    assert(c.modelUrl == "http://a/nodes/description/model/tree?variant=simple");
    assert(near(c.offset, V{.5, -1, .25}) && near(c.yawDeg, 90) && near(c.scale, .5));
    assert(!parseConfig("").enabled && !parseConfig("model=http://x/m\n").enabled);
    assert(near(parseConfig("url=http://a\nscale=-3\n").scale, 1));
    puts("PASS config file");
}

static void testHttpFraming() {
    Url u;
    assert(parseUrl("http://erid:8080/nodes/sim/x?y=1", u) && u.host == "erid" && u.port == "8080" &&
           u.path == "/nodes/sim/x?y=1");
    assert(parseUrl("http://10.0.0.2", u) && u.port == "80" && u.path == "/");
    assert(parseUrl("http://[::1]:81?q", u) && u.host == "::1" && u.port == "81" && u.path == "/?q");
    for (const char *bad : {"https://a/", "http://", "http://a:x/", "http://u:p@a/", "http://a/b c"})
        assert(!parseUrl(bad, u));
    ResponseHead h;
    assert(parseHead("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nX: y", h) && h.status == 200 && h.chunked);
    assert(parseHead("HTTP/1.0 404 Not Found\r\ncontent-length: 12", h) && h.status == 404 && h.length == 12);
    // Chunked, fed one byte at a time, with a chunk extension and a trailer.
    std::string wire = "5;ext=1\r\nhello\r\n7\r\n world\n\r\n0\r\nX-T: 1\r\n\r\n", out;
    BodyDecoder d;
    h = {};
    h.chunked = true;
    d.reset(h);
    for (char c : wire)
        assert(d.feed(&c, 1, out));
    assert(d.done() && out == "hello world\n");
    d.reset(h);
    out.clear();
    assert(!d.feed("zz\r\n", 4, out));
    // Identity with and without length.
    h = {};
    h.length = 3;
    d.reset(h);
    out.clear();
    assert(d.feed("abcdef", 6, out) && out == "abc" && d.done());
    h.length = -1;
    d.reset(h);
    out.clear();
    assert(d.feed("abc", 3, out) && d.feed("def", 3, out) && out == "abcdef" && !d.done());
    // Lines across chunk boundaries, CRLF, SSE data: prefix, comments, blanks.
    LineSplitter ls;
    std::vector<std::string> lines;
    auto collect = [&](const std::string &l) { lines.push_back(l); };
    assert(ls.feed("{\"a\":1}\r\n{\"b\"", collect) && ls.feed(":2}\n\n: ping\ndata: {\"c\":3}\n", collect));
    assert(lines.size() == 3 && lines[1] == "{\"b\":2}" && lines[2] == "{\"c\":3}");
    puts("PASS HTTP framing (URL, head, chunked/identity bodies, line splitting)");
}

static void testSceneAndStale() {
    Model m;
    assert(parseModel(parse(kModel), m));
    State s;
    assert(parseState(parse(R"({"base": [0,0,0,0,0,0,1], "joints": {"shoulder": 0.3},
        "overlays": {"left_target": [0.5, 0, 1, 0, 0, 0, 1], "limited": true}})"),
                      s));
    Placement pl;
    teleop::Wire w;
    buildScene(w, &m, &s, pl, false);
    assert(w.segs.empty()); // Not placed yet.
    pl.reset({0, 0, 1.6}, M{});
    buildScene(w, &m, &s, pl, false);
    size_t fresh = w.segs.size();
    assert(fresh > 100);
    // The base box is drawn around the placement point (origin 1.6 m ahead, on the floor).
    bool boxNearOrigin = false;
    for (const auto &sg : w.segs)
        if (near(sg.color, V{.2, .4, .6}) && norm(sg.a - V{1.6, 0, .1}) < .4)
            boxNearOrigin = true;
    assert(boxNearOrigin);
    // Limited: a red ring is drawn; stale: same geometry, every colour dimmed.
    bool red = false;
    for (const auto &sg : w.segs)
        red = red || (sg.color.x > .9 && sg.color.y < .2);
    assert(red);
    teleop::Wire dim;
    buildScene(dim, &m, &s, pl, true);
    assert(dim.segs.size() == fresh);
    for (size_t i = 0; i < fresh; i++) {
        assert(near(dim.segs[i].a, w.segs[i].a));
        assert(near(dim.segs[i].color, w.segs[i].color * kStaleDim, 1e-12));
    }
    // No model yet: base axes only. Separate-stream objects are drawn when the state has none.
    teleop::Wire bare;
    buildScene(bare, nullptr, &s, pl, false);
    std::vector<Object> extra(1);
    extra[0].pose.p = {2, 0, 0};
    extra[0].geoms.push_back(m.visuals["base"][0]);
    teleop::Wire withObjects;
    buildScene(withObjects, nullptr, &s, pl, false, &extra);
    assert(withObjects.segs.size() == bare.segs.size() + 12);
    // Freshness and HUD text.
    Snapshot snap;
    assert(hudLines(snap, 0)[0] == "ROBOT AR CONNECTING");
    snap.error = "connection refused";
    assert(hudLines(snap, 0)[0] == "ROBOT AR CONNECTION REFUSED");
    snap.state = std::make_shared<State>(s);
    snap.model = std::make_shared<Model>(m);
    snap.lastStateNs = 1000000000LL;
    snap.fps = 30;
    assert(!isStale(snap, 1000000000LL + 400000000LL) && isStale(snap, 1000000000LL + 600000000LL));
    auto hud = hudLines(snap, 1000000000LL + 33000000LL);
    assert(hud[0] == "ROBOT AR OK 30 HZ 33 MS" && hud[1] == "LIMITED ");
    hud = hudLines(snap, 1000000000LL + 2500000000LL);
    assert(hud[0] == "ROBOT AR STALE 2.5 S" && hud[1] == "CONNECTION REFUSED");
    assert(hudText("joint limit: q3!") == "JOINT LIMIT  Q3 ");
    puts("PASS scene wireframe (placement, limited cue, stale dimming, objects) and HUD status");
}

// Drive the real client against a server; the Python test controls the server.
static int client(const char *base, const char *mode) {
    Config c = parseConfig(std::string("url=") + base + "\n" +
                           (std::strcmp(mode, "objects") == 0
                                ? std::string("objects=") + base + "/nodes/sim/objects/stream?format=jsonl&hz=10\n"
                                : ""));
    Client cl;
    int64_t t0 = nowNs();
    cl.start(c);
    auto wait = [&](double seconds, auto &&ok) {
        int64_t until = nowNs() + int64_t(seconds * 1e9);
        while (nowNs() < until) {
            Snapshot s = cl.snapshot();
            if (ok(s))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    };
    // Snapshots are cheap and never block on the network.
    for (int i = 0; i < 1000; i++) {
        int64_t a = nowNs();
        cl.snapshot();
        assert(nowNs() - a < 5000000);
    }
    if (std::strcmp(mode, "stats") == 0 || std::strcmp(mode, "objects") == 0) {
        bool ok = wait(5, [](const Snapshot &s) { return s.model && s.state && s.fps > 20; });
        Snapshot s = cl.snapshot();
        printf("connected=%d states=%llu fps=%.1f model=%s joints=%zu markers=%zu objects=%zu/%zu first=%.0fms\n",
               s.connected, (unsigned long long)s.states, s.fps, s.model ? s.model->version.c_str() : "-",
               s.state ? s.state->joints.size() : 0, s.state ? s.state->markers.size() : 0,
               s.state ? s.state->objects.size() : 0, s.objects ? s.objects->size() : 0, (nowNs() - t0) / 1e6);
        if (std::strcmp(mode, "objects") == 0)
            ok = ok && wait(3, [](const Snapshot &s) { return s.objects && s.objects->size() == 2; });
        if (!ok)
            return 1;
        // FK over the streamed state: every link with visuals is reachable.
        Snapshot n = cl.snapshot();
        auto links = forwardKinematics(*n.model, *n.state);
        for (const auto &kv : n.model->visuals)
            if (!links.count(kv.first))
                return 2;
        Placement pl;
        pl.reset({0, 0, 1.6}, M{});
        teleop::Wire w;
        buildScene(w, n.model.get(), n.state.get(), pl, false, n.objects.get());
        printf("segments=%zu\n", w.segs.size());
        return w.segs.size() > 300 ? 0 : 3;
    }
    // Scripted scenario: print events as they happen, one per line, for the Python driver.
    std::string lastVersion;
    long long lastReset = -1;
    bool stale = false;
    uint64_t lastStates = 0;
    int64_t end = nowNs() + int64_t(std::atof(mode) * 1e9);
    while (nowNs() < end) {
        Snapshot s = cl.snapshot();
        if (s.model && s.model->version != lastVersion) {
            lastVersion = s.model->version;
            printf("model %s fetches=%llu\n", lastVersion.c_str(), (unsigned long long)s.modelFetches);
        }
        if (s.state && s.state->hasResetSeq && s.state->resetSeq != lastReset) {
            lastReset = s.state->resetSeq;
            printf("reset_seq %lld\n", (long long)lastReset);
        }
        bool nowStale = s.state && isStale(s);
        if (nowStale != stale) {
            stale = nowStale;
            printf("%s states=%llu hud=%s\n", stale ? "stale" : "fresh", (unsigned long long)s.states,
                   hudLines(s)[0].c_str());
        }
        if (s.states > lastStates + 30) {
            lastStates = s.states;
            printf("tick states=%llu connected=%d\n", (unsigned long long)s.states, s.connected);
        }
        fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    int64_t a = nowNs();
    cl.stop();
    printf("stopped in %.0f ms\n", (nowNs() - a) / 1e6);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && std::strcmp(argv[1], "--client") == 0)
        return client(argv[2], argv[3]);
    testJson();
    testModel();
    testStateAndFk();
    testPlacement();
    testConfig();
    testHttpFraming();
    testSceneAndStale();
    puts("PASS robot AR core");
}
