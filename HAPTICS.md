# Haptics: controller vibration from the teleop node

The robot_utils `quest_teleop` node turns teleop events (armed, disarmed, held, resumed, disabled), the WBC's
per-arm limits and gripper contact into vibration cues (robot_utils docs/quest_teleop_node.md "Haptics": the cue
table, the toggles). The native app plays them: `native/haptics.h` (parsing, scheduling, the stream client; no
OpenXR) and `telemetry.cpp` (`xrApplyHapticFeedback` / `xrStopHapticFeedback` on a `VIBRATION_OUTPUT` action bound
to `/user/hand/{left,right}/output/haptic`). Core OpenXR: no Android permission, no runtime APK change.

## Channel

- **URL:** with `quest run --ar=URL`, `<url>/nodes/quest_teleop/haptics?stream=1`; `--ar-haptics=URL` overrides,
  `--ar-haptics=off` disables. Without `--ar`: no haptics.
- **Stream:** one long `GET` (chunked NDJSON, the AR state stream's pattern). The node writes a line on every change,
  and a keepalive every 0.2 s while anything plays, 1 s idle. No byte for 2 s: reconnect (250 ms doubling to 2 s;
  10 s after a 404, i.e. no teleop node behind that frontend).
- **Latency:** event -> buzz = the node's write + the one-way network + at most one XR frame (the player ticks
  every frame, 14 ms at 72 Hz). No poll period.

## The line (`teleop.quest_haptics.v1`)

```
{"schema", "epoch", "seq", "ttl_s",
 "hands": {"left"|"right": {"level": 0..1, "frequency_hz", "pulses": [{"id", "group", "amplitude", "duration_s",
                                                                       "frequency_hz", "delay_s", "age_s"}]}}}
```

- **Pulses** play once each: ids increase for the node's life, `epoch` names the life (a new epoch resets the
  memory). A pulse starts `delay_s` after its line arrives; one older than 0.5 s on arrival (`age_s`) is skipped
  (acked, not played). A newer `group` on a hand drops that hand's pulses still waiting. `duration_s` and
  `delay_s` are capped at 2 s; `frequency_hz` 0 = the runtime's default.
- **Level:** a continuous vibration while > 0, valid `ttl_s` (capped at 1 s) after its line arrives; played in
  100 ms pieces re-applied every 50 ms, so it ends within 100 ms of its TTL. Pulses play over it; it resumes after.
- **Lost stream:** levels and waiting pulses are dropped at once (a level gets a stop).
- **Unfocused:** commands are dropped (the runtime plays nothing); a level comes back on focus.

## The ack

Each telemetry record carries `"haptics": {"schema", "epoch", "last_id", "connected"}` while haptics are on:
`last_id` the newest pulse id accepted, `connected` the stream up. The node shows the round trip (`ack_ms`).
An older build has no block.

## Diagnosing

`ssh quest 'logcat -d | grep QuestRuntimeXR | grep -i haptic'`: `Haptics: <url>` at start-up, `Haptics stream:
ok` / an error on every change (`HAPTICS HTTP 404`: no quest_teleop node there; `CONNECTION REFUSED`: the
frontend), `Haptic feedback failed: <XrResult>` once per change of result. robot_utils' fake headset
(`tools/quest_teleop/fake_headset.py --haptics URL`) has a port of the player.

## Tests

`python3 test_haptics.py` (host, g++): each pulse once, the pair's timing, groups replacing waiting pulses, late
pulses, a level's pieces and TTL, a pulse over a level, epochs, disconnect, parsing and clamping; and the stream
client against a local chunked server, cut once: each pulse once across the reconnect, the level stopped, the
block. `test_robot_ar.py`: the URL's default and `off`.
