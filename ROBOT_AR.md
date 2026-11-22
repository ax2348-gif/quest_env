# Robot AR view

The headset draws the robot (the robot_utils MuJoCo simulator today, the real
robot later) as a wireframe over passthrough, live, so the operator can walk
around during whole-body teleop tests without looking at a laptop. It runs
alongside the telemetry stream and either teleop scheme. The telemetry records
and `control_intent` are unchanged: the view only reads the head pose and the
A button (as a reset press, see Placement). Y is not used by the view (on the
robot it is the robot client's enable, X its disable).

```sh
qp
qr --scheme=body-frozen --ar=http://HOST[:PORT]        # HOST = the robot frontend (port 80 by default)
```

`--ar` (alias `--sim-ar`) is the robot frontend's root URL. The headset then reads:

| What | Default URL | Override |
| --- | --- | --- |
| Description | `<url>/nodes/description/model/tree?variant=simple` | `--ar-model=URL` |
| State stream | `<url>/nodes/sim/robot_state/stream?format=jsonl&hz=30` | `--ar-state=URL` |
| Objects stream (optional) | objects inside the state lines | `--ar-objects=URL` |

The state URL is configurable so the same view works with the real robot's
`robot_state.v1` source. `--ar-offset=X,Y,Z` (metres, in the placed frame),
`--ar-yaw=DEG` and `--ar-scale=S` shift, turn or shrink the whole scene.
URLs are plain `http://` (no TLS, no credentials). A launch without `--ar`
turns the view off. The options are written to
`~/.config/quest-env/robot-ar` (one `key=value` per line) and read once at
native start-up; `-s` (the local kinematic demo) can be combined, but both
robots are then placed in front of the operator (the demo along the operator
forward, the AR scene along the head yaw), usually overlapping.

## What is drawn

- Each link's primitive visuals (box, cylinder, capsule, sphere) at the pose
  given by forward kinematics on the Quest: root link at the state's `base`
  pose, then each joint's origin and motion (revolute/continuous about the
  axis, prismatic along it, fixed; mimic joints follow their leader; joints
  missing from a state line read 0). Meshes are skipped in v1.
- Colours come from the description's `rgba`; very dark colours are lifted to
  grey to stay visible over passthrough.
- Overlays: target poses as small axes plus a green box (red when limited),
  commanded poses as larger axes, any other pose as axes; a base velocity hint
  as a floor arrow (1 s ahead, capped at 0.6 m). When `limited` is set, a red
  ring on the floor around the base and the HUD line `LIMITED <REASON>`.
- Scene objects from the state lines (`objects`), or from the separate
  objects stream when the state lines carry none.
- A small floor cross at the sim world origin.

## Placement

Every placement uses the head at that moment: the sim world origin sits 1.6 m
ahead of the head along the head's gravity-aligned yaw and 1.6 m below the head
(a hypothetical floor, not detected ground); the sim's +x is that yaw. The
operator forward and A's realignment never feed it. Looking straight down or
up, the headset's up/down vector gives the facing. Sim world: z up, metres.
Operator space: the demo's FLU frame of OpenXR LOCAL.

The scene is placed:

- at the **first tracked head**, and again after a **LOCAL reference-space
  change**;
- on a **reset**, which is:
  - a change of `reset_seq` in the state lines (the sim's reset). The first
    value seen is only a baseline; a missing or unchanged `reset_seq` never
    re-anchors. A change seen while the head is untracked is applied once
    tracking returns.
  - an **A** press (rising edge, XR input focused, head tracked) when the state
    lines carry no `reset_seq` (the real robot: there A only re-anchors the AR
    scene). With `reset_seq` present, A does nothing here.

**Y** does nothing to the scene. In the standard scheme A also realigns the
operator forward (unchanged); in body-frozen it does not (see DEMO.md).
Logcat: `Robot AR re-anchored at head yaw (reset_seq|A)`.

## Status and robustness

A HUD line under the demo text shows `ROBOT AR OK <rate> HZ <age> MS`,
`ROBOT AR STALE <age> S` (plus the last error) or `ROBOT AR CONNECTING` /
`ROBOT AR <ERROR>`; with a state and no description yet it adds `NO MODEL`.
The rate is state lines received per second; the age is the time since the
newest line arrived on the Quest (not end-to-end latency).

Networking runs on background threads. The XR frame loop only copies the newest
snapshot (shared pointers under a mutex) and never waits on the network.
After 0.5 s without a state line the last pose stays, drawn dimmed. A stream
that is silent for 2 s, closed or refused is reconnected with backoff
(0.25–2 s). The description is fetched at every connect and again whenever a
state line's `model_version` differs (at most every 2 s while it keeps
differing). Connection changes are logged to logcat (`Robot AR ...`).

## Expected endpoint formats

The parser is tolerant; these are the shapes it is tested with.

Description (`GET .../model/tree?variant=simple`):

```json
{"model_version": "a1b2", "root": "base_link",
 "links": [{"name": "base_link", "visuals": [
     {"type": "box", "size": [0.6, 0.6, 0.1], "origin": {"pos": [0, 0, 0.2], "quat": [0, 0, 0, 1]},
      "rgba": [0.7, 0.8, 0.8, 1]}]}],
 "joints": [{"name": "arm_joint1", "type": "revolute", "parent": "base_link", "child": "arm_link1",
             "origin": {"pos": [0, 0.25, 1.0], "quat": [0, 0, 0, 1]}, "axis": [0, 0, 1],
             "lower": -2.8, "upper": 2.8, "mimic": {"joint": "...", "multiplier": 1, "offset": 0}}]}
```

- `links` may also be an object keyed by name; `visuals` may be one object.
  A visual's geometry may sit under `geometry`. `type`/`shape`/`geom`/`kind`.
- Poses: `origin`/`pose` objects with `pos`|`xyz`|`position` and `quat`
  (x, y, z, w) or `rpy` (URDF fixed-axis), or a 7-array `[x,y,z,qx,qy,qz,qw]`.
  A joint without an origin takes it from a GET /model style `frames[child]`.
- Sizes: box `size` = full extents (URDF) or `half_size`/`half_extents`;
  sphere `radius`; cylinder/capsule `radius` + `length` (full length of the
  straight part, along local z) or MuJoCo `size: [radius, half_length]`.
- `root` (or `base_link`); otherwise the link that is nobody's child.

State line (`robot_state.v1`):

```json
{"t": 12.3, "seq": 369, "model_version": "a1b2", "reset_seq": 2, "base": [x, y, z, qx, qy, qz, qw],
 "joints": {"arm_joint1": 0.4, "module_0_steer": 0.1},
 "overlays": {"wbc": {"left_target": {"pose": [7 numbers], "limited": false},
                      "left_command": [7 numbers], "limited": true, "reason": "joint limit",
                      "base_mode": "follow", "base_hint": [vx, vy, wz]}},
 "objects": {"cube": {"pose": [7 numbers], "geom": {"type": "box", "size": [0.05, 0.05, 0.05]}}}}
```

Overlays are free-form and walked recursively: a 7-array or an object with
`pose` is a frame (role from the key: `target`, `command`/`cmd`, otherwise a
plain frame; world frame unless the object says `"frame": "base"`); a key with
`hint`/`velocity`/`twist` holding 2–3 numbers is a base-frame velocity
`[vx, vy, wz]`; `limited` booleans, `reason` and `*mode` strings feed the cue
and HUD. Lines may be plain JSON lines (chunked or close-delimited) or SSE
`data:` lines. A separate objects stream sends `{"objects": {...}}` lines.

## Host validation

```sh
python3 test_robot_ar.py      # parsing, FK, placement, staleness, HTTP framing, client vs fake server
python3 test_launch.py        # --ar options -> robot-ar config, rejection of bad values
python3 test_native_demo.py   # Linux host with EGL/GLES: demo + robot AR previews
python3 tools/fake_robot_server.py --port 8765 --objects-inline   # a stand-in robot
```

`tools/fake_robot_server.py` serves a swerve base driving a slow circle, two
waving arms with mimic fingers, EE target/command overlays, a base hint and a
`limited` flag toggling every 5 s and a `reset_seq`, plus `/fake/bump` (new
`model_version`), `/fake/reset` (`reset_seq` + 1), `/fake/pause` and
`/fake/resume`; `--no-reset-seq` omits `reset_seq` like the real robot. `test_native_demo.py` renders the robot from
it through the real renderer: `/tmp/quest-ar-eye-{1,2}.ppm`, and dimmed
`/tmp/quest-ar-stale-eye-{1,2}.ppm`.

Only the headset can confirm: Wi-Fi reachability of the robot from the Quest,
perceived scale and placement, readability over passthrough, frame rate with
the extra geometry, and that the frame loop stays smooth through outages.
