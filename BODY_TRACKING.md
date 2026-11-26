# Body tracking

`qr --body` (or `q run --body`) forwards Meta's body tracking, the operator's tracked
skeleton, in every telemetry record as a `body` block. The headset forwards the runtime's
data as is: every joint's pose and location flags, the joint set, the skeleton change count,
confidence and fidelity. Nothing on the headset uses it, and neither the intent nor the
other blocks change.

It is **opt-in**: a launch without `--body` creates no body tracker, enables no body
extension and sends no `body` block. Tracking the body costs the headset camera and CPU
time; it's off unless a consumer wants it.

## Requirements

- **Runtime 1.7.0 or later** (`quest-dev-runtime`). It declares
  `com.oculus.permission.BODY_TRACKING`. The native library runs in the runtime's own `:xr`
  process (`android.app.NativeActivity`, not the BlackBox container), so it has exactly the
  runtime APK's permissions.
- **The permission granted.** `BODY_TRACKING` is a runtime permission. Grant it in one of
  three ways:
  - the setup wizard grants it over USB (`adb shell pm grant dev.quest.runtime com.oculus.permission.BODY_TRACKING`);
  - opening Quest developer runtime from Library → Unknown sources asks for it once;
  - its "Allow body tracking" button asks again.
  Horizon OS can also need *Settings → Movement tracking → Body tracking* switched on.
- Quest 3 / 3S for high fidelity (inside-out body tracking) and the full-body (legs) joint set.

Without the permission, or on an older runtime, the record carries
`"body":{"schema":"teleop.quest_body.v1","available":false,"reason":"..."}`, and the tracker
is retried every 5 s. A permission granted while telemetry runs takes effect without a
restart.

## Options

| Option | Effect |
|---|---|
| `--body`, `--body=core` | track; send the core joints (all but the 48 finger joints) |
| `--body=all` | send every joint of the set |
| `--body=off` | the default: no tracker |
| `--body-set=full_body` / `default` | joint set: the full-body set (84 joints, with legs) when the runtime has `XR_META_body_tracking_full_body`; the default set (70 joints, upper body) otherwise or when asked (default `full_body`) |
| `--body-fidelity=high` / `low` / `none` | the fidelity to request with `XR_META_body_tracking_fidelity` (default `high`); `none` requests nothing |
| `--body-every=N` | the block in every Nth record only (1–999, default 1) |

The script writes these options to `~/.config/quest-env/body-tracking`, one `key=value` per line:
`joints`, `joint_set`, `fidelity`, `every`. The native app reads the file once at start.

## Extensions

The app enables each extension only when the runtime lists it, and each one degrades on its
own, like hand tracking:

- `XR_FB_body_tracking`: required. Without it, the record says `available:false`.
- `XR_META_body_tracking_full_body`: the full-body joint set. If the full-body tracker fails,
  the app creates a default-set tracker instead.
- `XR_META_body_tracking_fidelity`: requests the configured fidelity and reports the one in
  force each frame. This extension is in Meta's preview headers, not in Khronos 1.1.53, so
  `telemetry.cpp` declares its types.

`XR_META_body_tracking_calibration`, the user's height override, is not used.

## The `body` block (schema `teleop.quest_body.v1`)

The block sits in the `teleop.quest_state.v1` record next to `head` and `hands`. It is
additive: consumers that don't know it ignore it.

```json
"body": {
  "schema": "teleop.quest_body.v1",
  "available": true,
  "active": true,
  "joint_set": "full_body",
  "joint_set_id": 1000274000,
  "joint_count": 84,
  "confidence": 0.93,
  "fidelity": "high",
  "fidelity_requested": "high",
  "skeleton_changed_count": 2,
  "time_ns": 1234567890123,
  "subset": "core",
  "joints": [0, 1, 2, ..., 19, 44, 45, 70, ..., 83],
  "flags": [15, 15, ...],
  "poses": [x, y, z, qx, qy, qz, qw, ...]
}
```

| Field | Meaning |
|---|---|
| `available` | A body tracker exists. If false, the block holds only `schema`, `available` and `reason`. |
| `reason` | Why there's no tracker: the extension is missing, the system doesn't support it, or creation failed (with the `XrResult` and the permission state). |
| `active` | `XrBodyJointLocationsFB.isActive`. If false, `joints`, `flags` and `poses` are empty. |
| `joint_set`, `joint_set_id` | `default` (`XR_BODY_JOINT_SET_DEFAULT_FB`, 0) or `full_body` (`XR_BODY_JOINT_SET_FULL_BODY_META`, 1000274000). |
| `joint_count` | The number of joints in the set: 70 or 84. |
| `confidence` | `XrBodyJointLocationsFB.confidence`, 0–1. |
| `fidelity` | The fidelity in force, `high` or `low`; null when not reported (no fidelity extension, or inactive). |
| `fidelity_requested` | The fidelity requested at tracker creation; null when none was requested. |
| `skeleton_changed_count` | Increments when the runtime changes the skeleton's proportions. Re-read your bone lengths when it does. |
| `time_ns` | `XrBodyJointLocationsFB.time`, the body sample's own XrTime. It can lag the record's `sample_time_ns`. |
| `subset` | `core` or `all`. |
| `joints` | The joint ids sent, ascending, from the set's enum (`XrBodyJointFB` / `XrFullBodyJointMETA`). |
| `flags` | Per joint, the `XrSpaceLocationFlags`: 1 orientation valid, 2 position valid, 4 orientation tracked, 8 position tracked. 15 is fully tracked; 3 is valid but inferred. |
| `poses` | Per joint, 7 numbers: the position in metres, then the quaternion (x, y, z, w), in OpenXR **LOCAL**. That's the same space as `head.pose` and `hands.*.pose` (`reference_space: "local"`). Values have 4 decimals. A joint with non-finite values is sent as zeros with flags 0. |

Joint ids 0–17 are the same in both sets:

| Id | Joint |
|---|---|
| 0 | root |
| 1 | hips |
| 2–4 | spine lower, middle, upper |
| 5 | chest |
| 6 | neck |
| 7 | head |
| 8–12 | left shoulder, scapula, arm upper, arm lower (the **elbow**), wrist twist |
| 13–17 | right shoulder, scapula, arm upper, arm lower, wrist twist |

The other ids:

| Ids | Joints | In `core` |
|---|---|---|
| 18–19 | left palm, wrist | yes |
| 20–43 | left fingers | no |
| 44–45 | right palm, wrist | yes |
| 46–69 | right fingers | no |
| 70–83 | legs (full-body set only): upper leg, lower leg, ankle twist, ankle, subtalar, transverse, ball; left then right | yes |

## Cost

The `core` block of the default set is about 1.2 kB per record: 22 joints. `all` is about
3.3 kB: 70 joints. At 72 records/s, that adds about 90 kB/s or 240 kB/s.

`--body-every=N` divides the rate. Native work is one `xrLocateBodyJointsFB` per record
and the JSON formatting.

## Headset on the neck

Meta's body model assumes the headset is on the head. Our operators wear it on the neck as a
torso proxy (robot_utils), so the solver sees a "head" about 25 cm low. That probably
displaces the shoulders and elbows, and could lower the confidence. This is unverified. See
"On-device check" in robot_utils `docs/quest_teleop_node.md`.

## Validation

`python3 test_body_tracking.py` checks the configuration parsing, the joint subsets and the
JSON on the host. It also checks the golden `fixtures/body_frames.jsonl`, which robot_utils'
reader tests use a copy of. `test_launch.py` covers the `--body` options and their file.

The OpenXR calls, the permission and real data are **not yet verified on a headset**.
