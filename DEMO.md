# Local bimanual teleoperation demo

Update through Git on Quest (no APK update or ADB):

```sh
qp
qr -s
```

Keep **Seamless Multitasking enabled** for the terminal to coexist with controller
input. Use Meta to return input to XR. Release both grips once after startup.

| Input / command | Behavior |
| --- | --- |
| Left / right grip | Independently clutch that arm; release holds its joints |
| Left / right index trigger | Continuously close that gripper |
| Left stick | Forward/back and lateral base motion |
| Right stick X | Base yaw |
| A (both grips released) | Align operator forward to current headset facing, yaw only |
| Y | Reset the demo pose/placement; release and re-grip to resume |
| B held 2 seconds | Existing telemetry shutdown behavior |
| `qr -s --base-frame=frozen` | Keep clutched targets in the captured world frame while base moves |
| `qr -s --no-viewer` | Demo without the terminal viewer |
| `qr` | Normal telemetry, demo disabled |
| Ctrl-C in launch-owned viewer | Stop native telemetry and demo |
| q/Esc in viewer | Detach viewer; native app continues |

The robot is **OpenArm-inspired**, with two 7-revolute-joint arms, bent starting
postures, 36 cm upper arms and 33.6 cm forearms (120% of the original arm scale). It is a stylized demonstration,
not an exact OpenArm URDF or hardware calibration. All arm joints are limited to
±2.85 rad with a 2.5 rad/s kinematic step bound. Damped Jacobian IK tracks position
and full orientation, with null-space posture/manipulability preference and a
basic capsule self-clearance guard. Unreachable targets can stop short; release
and re-clutch. There are no dynamics, contact physics, scene meshes or real-world
collision checks.

Cyan = left arm, amber = right. Tool axes: red X, green Y, blue Z. Small target
frames appear while clutched. The base uses ideal four-module swerve kinematics: each wheel steers to its local
velocity (including base yaw) and rolls at the corresponding signed speed.
Steering takes the shorter turn, reversing wheel speed when appropriate.
There are no tire forces or steering dynamics. The beveled chassis supports a
15–18 cm diameter round mast, compared with 6.5 cm arm tubes.

The base arrow shows robot forward. A virtual ground
grid stays fixed as the robot moves. Placement is 1.6 m along the calibrated operator forward
(default OpenXR LOCAL -Z) from the first tracked head position, with ground 1.6 m below that head
position. This is a hypothetical floor, not detected AR ground. Y resets placement
using the current head position and calibrated operator heading. A changes the
control mapping only: it does not rotate or teleport the existing robot/grid.
A LOCAL reference-space change resets the demo and invalidates clutch anchors.

## Frame mapping

Operator and robot bases use **X forward, Y left, Z up**. OpenXR LOCAL maps to
operator coordinates through `C = [[0,0,-1],[-1,0,0],[0,1,0]]`. Neither headset yaw
nor controller orientation continuously redefines operator front. Press A with
both grips released to capture headset yaw as the new forward direction, keeping
up aligned with gravity. A is ignored while either clutch is engaged, while a
grip is held, without tracked head pose, or when looking almost straight up/down.
Rejected presses are not queued; release the grips and press A again. The setting
is session-local and resets on native restart or LOCAL reference-space change.

On each grip engagement, record controller pose `(p0, R0)` in operator base and
measured EE pose `(e0, E0)` in robot base. During the engagement:

```text
dp = p_now - p0                 # limited to 0.45 m per engagement
dR = R_now * transpose(R0)      # spatial rotation; no angle clamp
EE_target_in_base = (e0 + dp, dR * E0)
EE_target_world = selected_robot_base * EE_target_in_base
```

Default `live` uses the current robot base. `frozen` uses its pose at engagement.
The EE anchor is measured before integrating base motion on that frame. Released
arms keep joint angles fixed, even when the base drives. Trigger/gripper and base
motion are independent of arm clutches. Sticks have an 18% dead zone; full-scale
translation is 0.45 m/s and yaw 0.8 rad/s.

## Native control intent, separate from raw telemetry

Every existing `teleop.quest_state.v1` record still contains its original raw
head/controller poses, buttons and tracking fields. It now also contains:

- `control_intent`: schema `teleop.operator_intent.v1`, session ID,
  `frame: operator_base_flu`, `translation_limit_m`, `base_velocity` and `hands`.
  `operator_from_quest` is the current 9-element row-major basis rotation and
  `operator_frame_revision` increments on successful A realignment.
- Each intent hand: `valid`, `engaged`, `needs_release`, `engagement_id`,
  `delta_position` (3 metres), `delta_rotation` (9 row-major matrix entries),
  and `gripper` (0 open–1 closed). Base velocity is robot-base forward/left m/s
  and yaw rad/s. These are **requests**, not measured robot motion.
- `demo`: enabled/base-frame mode; when enabled, placement, base position in the
  operator world, per-arm joint angles and position/orientation errors.

Intent computation runs in normal telemetry mode too. Only `-s` instantiates the
stereo demo rendering resources and advances robot kinematics. The demo directly
consumes the same native intent; it does not open another TCP client or alter raw
controller measurements. Up to eight independent TCP readers remain supported.

Grip engages at >=0.6, releases at <=0.4. Startup, focus/tracking loss, native frame
gaps >200 ms and reference-space/session changes require a valid grip release
before engaging. Invalid commands disengage arms and stop affected base inputs;
gripper values are held but must be ignored when `valid=false`. Session changes
make old engagement IDs obsolete. IDs are unique only within a provider session.

A remote robot consumer must:

1. Check frame arrival freshness on its own clock (recommended timeout 200 ms),
   focus, command validity, session ID, operator-frame revision, and ordered `frame_seq`.
   A changed revision invalidates saved EE anchors just like a changed session.
2. On a fresh engagement, capture its **measured EE anchor** and robot base once.
   Apply the formula above, using the live/frozen base policy on the robot side.
3. If attaching/reconnecting mid-engagement, wait for release and a new engagement;
   do not apply an existing nonzero delta to a newly captured EE anchor.
4. On stale/disconnected/invalid/released input, hold arm joints and stop base
   motion. The native app cannot stop a remote robot when the connection is lost.

The Quest does not know a remote robot's measured EE pose. Anchor capture at the
robot happens upon receipt, with network delay; synchronized remote anchoring
would require robot feedback/timestamp history. No physical robot command
transport or remote IK is implemented here.

## Validation and limits

```sh
python3 tools/deliver.py native
python3 test_native_demo.py
python3 test_launch.py
python3 test_interrupt.py
python3 test_tui.py
python3 test_controller_server.py
sh quest verify
```

Host tests cover base-axis mapping, nontrivial anchor orientations, rotation near
180°, independent grips and invalidation, Jacobians against finite differences,
both-arm IK, live/frozen base motion, joint hold and unreachable targets. A real
GLES host renderer tests both eye images using mock OpenXR swapchains and
synthetic head poses. Its PPM previews are written under `/tmp/quest-demo-eye-*`.
This verifies shaders/projection/alpha and geometry, **not on-headset compositor
integration, perceived scale or headset performance**. The first Quest visual
check is pending; the headset was left charging and no ADB was used.

Rollback: `usable-2026-10-20` remains the earlier checkpoint. The previous
no-adjacent-placement library is commit `e9b74f3`.


## Experimental walking scheme: body-frozen

The baseline is tagged `initial-usable-teleop-v1` (50ae956). The original scheme
remains the default. Pull and try the alternative:

```sh
qp
qr -s --scheme=body-frozen
```

Each grip engagement independently records an upright operator frame (head
position and gravity-aligned headset yaw) and an upright robot shoulder frame
(current shoulder position and platform yaw). Headset yaw approximates torso
heading: looking sideways at engagement changes that hand's command axes.
These frames remain fixed in world space until that hand releases. No continuous
head/body recentering happens. A still calibrates placement/reset heading only
when both grips are released; new engagements in this scheme capture current
head yaw regardless of that calibration.

For each hand, transform the anchor and current controller into its captured
operator frame. Use `dp = current.p - anchor.p` and spatial rotation
`dR = current.R * anchor.R^T`. Capture the measured EE pose in the corresponding
robot shoulder frame and command `p = EE_anchor.p + dp`,
`R = dR * EE_anchor.R`, then transform back through the frozen shoulder frame.
Thus clutch engagement gives exactly the measured EE target. Moving the platform
does not add another displacement to that target. Equal operator/platform travel
leaves the relative arm target unchanged; operator travel alone moves the target
through the world. Release holds arm joints, so a moving platform carries the arm.
The old 0.45 m clutch displacement cap is disabled here to allow walking.
Tracking/session discontinuities still invalidate engagements and require release.

### Automove (left stick click)

Automove starts **off**, is available only in body-frozen mode, and is toggled by
left thumbstick click. The TUI reports enabled/recovering state. It translates the
base on the flat ground without changing its heading or frozen EE targets.

This prototype detects outer workspace extension, a cause of near-straight-arm
singularities. It starts above 88% of nominal shoulder-to-tool reach and stops
below 80%. It searches coarse planar directions, accepting only directions that
reduce extension pressure without materially worsening another engaged arm.
Incompatible bimanual targets or purely vertical overreach can therefore leave
it unable to help. It does not solve arbitrary wrist singularities, joint limits,
or collisions, and does not continuously optimize fine arm configurations.

Velocity is capped at 0.22 m/s and changes by at most 0.35 m/s² during normal
start, recovery, and toggle-off. Speed decreases near the recovery boundary;
hysteresis prevents repeated starts/stops. Manual joystick input takes precedence.
Release of all clutches, loss of valid input, and manual override stop automatic
motion immediately; those interruptions deliberately bypass the ordinary ramp.
The existing swerve animation follows combined manual/automatic base velocity.

`control_intent` adds `scheme` and each hand's captured operator base pose in
Quest LOCAL coordinates. `translation_limit_m` is null in body-frozen mode.
Raw tracking telemetry remains unchanged. Demo telemetry adds automove state and
world-space velocity. Remote robots must still capture their own measured EE and
base/shoulder frames; this demo never commands a real robot.

Host regression tests cover separate hand headings, zero initial target jump,
frozen anchors under robot rotation/translation, matching walking displacement,
uncapped travel, automove acceleration/speed limits, idle workspace behavior,
conflicting targets, and manual/tracking interruptions. Headset validation of this
experimental scheme remains to be done.
