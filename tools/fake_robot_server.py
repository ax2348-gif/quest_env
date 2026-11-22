#!/usr/bin/env python3
"""A stand-in for the robot's description + robot_state.v1 endpoints (host tests).

Serves, like the robot frontend (paths under /nodes/<node>):
  GET /nodes/description/model/tree?variant=simple   the description (primitives)
  GET /nodes/sim/robot_state/stream?format=jsonl&hz=30       robot_state.v1 lines (chunked)
  GET /nodes/sim/objects/stream?format=jsonl&hz=10           {"objects": {...}} lines
  GET /fake/bump                                             new model_version (taller mast)
  GET /fake/reset                                            reset_seq + 1 (the sim's reset: AR re-anchors)

A swerve base drives a slow circle; two 4-joint arms with mimic fingers wave;
overlays carry EE targets/commands, a base velocity hint and a `limited` flag
that toggles every 5 s. Standard library only.

  python3 tools/fake_robot_server.py --port 8765 [--close-delimited] [--objects-inline]
"""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import sys
import threading
import time
from urllib.parse import parse_qs, urlparse

STATE = {"version": 1, "reset_seq": 0}
LOCK = threading.Lock()


def quat_z(yaw):
    return [0.0, 0.0, math.sin(yaw / 2), math.cos(yaw / 2)]


def quat_x(angle):
    return [math.sin(angle / 2), 0.0, 0.0, math.cos(angle / 2)]


def origin(pos, quat=(0, 0, 0, 1)):
    return {"pos": list(pos), "quat": list(quat)}


def model():
    v = STATE["version"]
    mast = 0.55 + 0.1 * (v - 1)
    grey, cyan, amber, dark = [0.65, 0.8, 0.77, 1], [0.15, 0.85, 1, 1], [1, 0.62, 0.15, 1], [0.2, 0.2, 0.2, 1]
    links = [{"name": "base_link", "visuals": [
        {"type": "box", "size": [0.68, 0.64, 0.10], "origin": origin([0, 0, 0.23]), "rgba": grey},
        {"type": "cylinder", "radius": 0.08, "length": mast, "origin": origin([-0.06, 0, 0.28 + mast / 2]),
         "rgba": grey},
        {"type": "capsule", "radius": 0.05, "length": 0.5, "origin": origin([0, 0, 0.28 + mast], quat_x(math.pi / 2)),
         "rgba": grey}]}]
    joints = []
    for i, (x, y) in enumerate([(0.27, 0.27), (0.27, -0.27), (-0.27, 0.27), (-0.27, -0.27)]):
        m = f"module_{i}"
        links.append({"name": f"{m}_steer", "visuals": [
            {"type": "cylinder", "radius": 0.04, "length": 0.1, "origin": origin([0, 0, 0.22]), "rgba": grey}]})
        # Wheel axis is local y: the cylinder's z axis is turned onto y.
        links.append({"name": f"{m}_wheel", "visuals": [
            {"type": "cylinder", "radius": 0.075, "length": 0.05, "origin": origin([0, 0, 0], quat_x(math.pi / 2)),
             "rgba": dark},
            {"type": "box", "size": [0.12, 0.052, 0.01], "rgba": grey}]})
        joints.append({"name": f"{m}_steer", "type": "revolute", "parent": "base_link", "child": f"{m}_steer",
                       "origin": origin([x, y, 0]), "axis": [0, 0, 1], "lower": -3.14, "upper": 3.14})
        joints.append({"name": f"{m}_wheel", "type": "continuous", "parent": f"{m}_steer",
                       "child": f"{m}_wheel", "origin": origin([0, 0, 0.075]), "axis": [0, 1, 0]})
    for side, sign, colour in (("left", 1, cyan), ("right", -1, amber)):
        a = f"{side}_arm"
        names = [f"{a}_link{k}" for k in range(1, 5)]
        links += [
            {"name": names[0], "visuals": [{"type": "sphere", "radius": 0.06, "rgba": colour}]},
            {"name": names[1], "visuals": [{"type": "capsule", "radius": 0.035, "length": 0.30,
                                            "origin": origin([0, 0, -0.15]), "rgba": colour}]},
            {"name": names[2], "visuals": [{"type": "capsule", "radius": 0.032, "length": 0.28,
                                            "origin": origin([0, 0, -0.14]), "rgba": colour}]},
            {"name": names[3], "visuals": [{"type": "box", "size": [0.05, 0.11, 0.05], "rgba": colour}]},
            {"name": f"{a}_finger_a", "visuals": [{"type": "box", "half_size": [0.04, 0.008, 0.018],
                                                   "origin": origin([0, 0, -0.06]), "rgba": colour}]},
            {"name": f"{a}_finger_b", "visuals": [{"type": "box", "half_size": [0.04, 0.008, 0.018],
                                                   "origin": origin([0, 0, -0.06]), "rgba": colour}]},
        ]
        joints += [
            {"name": f"{a}_joint1", "type": "revolute", "parent": "base_link", "child": names[0],
             "origin": origin([0, sign * 0.25, 0.28 + mast]), "axis": [0, 1, 0], "lower": -2.8, "upper": 2.8},
            {"name": f"{a}_joint2", "type": "revolute", "parent": names[0], "child": names[1],
             "origin": origin([0, 0, 0]), "axis": [1, 0, 0], "lower": -2.8, "upper": 2.8},
            {"name": f"{a}_joint3", "type": "revolute", "parent": names[1], "child": names[2],
             "origin": origin([0, 0, -0.30]), "axis": [0, 1, 0], "lower": -2.8, "upper": 2.8},
            {"name": f"{a}_joint4", "type": "revolute", "parent": names[2], "child": names[3],
             "origin": origin([0, 0, -0.28]), "axis": [0, 0, 1], "lower": -2.8, "upper": 2.8},
            {"name": f"{a}_finger_a", "type": "prismatic", "parent": names[3], "child": f"{a}_finger_a",
             "origin": origin([0, 0.01, 0]), "axis": [0, 1, 0], "lower": 0, "upper": 0.04},
            {"name": f"{a}_finger_b", "type": "prismatic", "parent": names[3], "child": f"{a}_finger_b",
             "origin": origin([0, -0.01, 0]), "axis": [0, 1, 0], "lower": -0.04, "upper": 0,
             "mimic": {"joint": f"{a}_finger_a", "multiplier": -1, "offset": 0}},
        ]
    return {"model_version": f"fake-{v}", "root": "base_link", "variant": "simple", "links": links,
            "joints": joints}


def objects(t):
    return {"table": {"pose": [1.2, 0.0, 0.35, 0, 0, 0, 1],
                      "geom": {"type": "box", "size": [0.6, 1.0, 0.7], "rgba": [0.8, 0.7, 0.5, 1]}},
            "cube": {"pose": [1.0, 0.1 * math.sin(t), 0.73, *quat_z(t)],
                     "geom": {"type": "box", "size": [0.05, 0.05, 0.05], "rgba": [1, 0.3, 0.3, 1]}}}


def state(seq, t, objects_inline, reset_seq=True):
    radius, period = 0.5, 20.0
    w = 2 * math.pi / period
    yaw = w * t  # Starts at the origin facing +x, circling left.
    base = [radius * math.sin(w * t), radius * (1 - math.cos(w * t)), 0.0, *quat_z(yaw)]
    joints = {}
    for i in range(4):
        joints[f"module_{i}_steer"] = 0.0
        joints[f"module_{i}_wheel"] = (radius * w / 0.075) * t
    for side, sign in (("left", 1), ("right", -1)):
        a = f"{side}_arm"
        joints[f"{a}_joint1"] = -0.9 + 0.4 * math.sin(t)
        joints[f"{a}_joint2"] = sign * (0.3 + 0.2 * math.sin(0.7 * t))
        joints[f"{a}_joint3"] = -1.2 + 0.5 * math.sin(1.3 * t + sign)
        joints[f"{a}_joint4"] = 0.5 * math.sin(0.5 * t)
        joints[f"{a}_finger_a"] = 0.02 + 0.02 * math.sin(2 * t)
    c, s = math.cos(yaw), math.sin(yaw)

    def ahead(x, y, z):
        return [base[0] + c * x - s * y, base[1] + s * x + c * y, z, *quat_z(yaw)]

    limited = int(t / 5) % 2 == 1
    line = {"schema": "robot_state.v1", "t": round(t, 4), "seq": seq,
            "model_version": f"fake-{STATE['version']}", "base": [round(x, 6) for x in base],
            "joints": {k: round(v, 6) for k, v in joints.items()},
            "overlays": {"wbc": {"left_target": {"pose": ahead(0.55, 0.3, 1.0), "limited": False},
                                 "right_target": {"pose": ahead(0.55, -0.3, 1.0), "limited": limited},
                                 "left_command": ahead(0.5, 0.28, 0.98),
                                 "right_command": ahead(0.5, -0.28, 0.98),
                                 "limited": limited, "reason": "joint limit" if limited else "",
                                 "base_mode": "follow", "base_hint": [radius * w, 0.0, w]}}}
    if reset_seq:  # The sim's reset counter; the real robot sends none (--no-reset-seq).
        line["reset_seq"] = STATE["reset_seq"]
    if objects_inline:
        line["objects"] = objects(t)
    return line


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "FakeRobot/1"

    def log_message(self, *args):
        if self.server.verbose:
            super().log_message(*args)

    def send_json(self, obj):
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def stream(self, make, hz):
        close_delimited = self.server.close_delimited
        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson")
        self.send_header("Cache-Control", "no-store")
        if close_delimited:
            self.send_header("Connection", "close")
        else:
            self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        self.close_connection = True
        seq, start = 0, time.monotonic()
        try:
            while not self.server.stopping and (self.server.max_lines <= 0 or seq < self.server.max_lines):
                if self.server.paused.is_set():
                    time.sleep(0.05)
                    start += 0.05  # No catch-up burst on resume.
                    continue
                data = (json.dumps(make(seq, time.monotonic() - start)) + "\n").encode()
                if close_delimited:
                    self.wfile.write(data)
                else:
                    self.wfile.write(b"%x\r\n%s\r\n" % (len(data), data))
                self.wfile.flush()
                seq += 1
                time.sleep(max(0.0, start + seq / hz - time.monotonic()))
            if not close_delimited:
                self.wfile.write(b"0\r\n\r\n")
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        url = urlparse(self.path)
        query = parse_qs(url.query)
        hz = min(120.0, max(1.0, float(query.get("hz", ["30"])[0])))
        if url.path in ("/nodes/description/model/tree", "/nodes/description/model"):
            with LOCK:
                self.server.model_requests += 1
                return self.send_json(model())
        if url.path == "/nodes/sim/robot_state/stream":
            return self.stream(lambda seq, t: state(seq, t, self.server.objects_inline, self.server.reset_seq), hz)
        if url.path == "/nodes/sim/objects/stream":
            return self.stream(lambda seq, t: {"t": t, "seq": seq, "objects": objects(t)}, hz)
        if url.path == "/fake/reset":
            with LOCK:
                STATE["reset_seq"] += 1
                return self.send_json({"reset_seq": STATE["reset_seq"]})
        if url.path == "/fake/bump":
            with LOCK:
                STATE["version"] += 1
                return self.send_json({"model_version": f"fake-{STATE['version']}"})
        if url.path == "/fake/pause":   # Stream stays open but silent.
            self.server.paused.set()
            return self.send_json({"paused": True})
        if url.path == "/fake/resume":
            self.server.paused.clear()
            return self.send_json({"paused": False})
        if url.path == "/fake/stats":
            return self.send_json({"model_requests": self.server.model_requests})
        self.send_error(404)


def serve(port=0, close_delimited=False, objects_inline=False, max_lines=0, verbose=False, host="127.0.0.1",
          reset_seq=True):
    server = ThreadingHTTPServer((host, port), Handler)
    server.daemon_threads = True
    server.close_delimited, server.objects_inline = close_delimited, objects_inline
    server.max_lines, server.verbose, server.stopping = max_lines, verbose, False
    server.paused, server.model_requests = threading.Event(), 0
    server.reset_seq = reset_seq
    return server


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", type=int, default=8765)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--close-delimited", action="store_true", help="no chunked encoding; end body at close")
    p.add_argument("--objects-inline", action="store_true", help="put objects in the state lines")
    p.add_argument("--no-reset-seq", action="store_true", help="omit reset_seq, like the real robot (A re-anchors)")
    p.add_argument("--max-lines", type=int, default=0, help="end each stream after N lines (0 = never)")
    p.add_argument("--verbose", action="store_true")
    a = p.parse_args()
    server = serve(a.port, a.close_delimited, a.objects_inline, a.max_lines, a.verbose, a.host, not a.no_reset_seq)
    print(f"PORT {server.server_address[1]}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
