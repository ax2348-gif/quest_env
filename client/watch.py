#!/usr/bin/env python3
"""Read-only Quest telemetry dashboard. Python standard library only."""
import argparse
from collections import deque
import curses
import json
import math
import os
import select
import socket
import subprocess
import time


def focus_xr():
    """Resume the singleTask XR activity; do not call native run/restart."""
    try:
        check = subprocess.run(['quest-native', 'status'], capture_output=True,
                               text=True, timeout=3, check=True)
        status = json.loads(check.stdout)
        if not status.get('ok') or not status.get('xr_pids'):
            return False, 'Telemetry is stopped; use qr first.'
        result = subprocess.run(
            ['quest-launch', 'dev.quest.runtime/android.app.NativeActivity'],
            capture_output=True, text=True, timeout=5, check=True)
        if not json.loads(result.stdout).get('ok'):
            return False, 'XR resume request failed.'
        return True, 'XR resume requested; check XR: FOCUSED / actions: OK.'
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        return False, 'XR resume failed: ' + str(error)


class Stream:
    def __init__(self, host, port):
        self.address = (host, port)
        self.sock = None
        self.buffer = b''
        self.sample = None
        self.received = 0.0
        self.retry = 0.0
        self.frames = deque()
        self.status = 'Waiting for telemetry'
        self.bad = 0
        self.notice = 'Meta: switch input focus; keep this terminal visible.'

    def close(self):
        if self.sock:
            self.sock.close()
        self.sock = None
        self.buffer = b''
        self.sample = None
        self.frames.clear()

    def tick(self):
        now = time.monotonic()
        if self.sock is None:
            if now < self.retry:
                return
            self.retry = now + 1
            try:
                self.sock = socket.create_connection(self.address, timeout=0.2)
                self.sock.setblocking(False)
                self.status = 'Connected; waiting for samples'
            except OSError:
                self.close()
                self.status = 'Waiting for telemetry — start with q run in another shell'
                return
        try:
            # Bound each drain so keyboard and rendering remain responsive.
            for _ in range(16):
                if not select.select([self.sock], [], [], 0)[0]:
                    break
                data = self.sock.recv(65536)
                if not data:
                    raise ConnectionError('Telemetry disconnected; reconnecting')
                self.buffer += data
                while b'\n' in self.buffer:
                    line, self.buffer = self.buffer.split(b'\n', 1)
                    try:
                        sample = json.loads(line)
                        if not isinstance(sample, dict) or sample.get('schema') != 'teleop.quest_state.v1':
                            raise ValueError('Unsupported sample')
                        if not isinstance(sample.get('head'), dict) or not isinstance(sample.get('hands'), dict):
                            raise ValueError('Missing poses')
                    except (ValueError, UnicodeError):
                        self.bad += 1
                        continue
                    if self.sample and sample.get('session_id') != self.sample.get('session_id'):
                        self.frames.clear()
                    self.sample = sample
                    self.received = now
                    self.frames.append(now)
                if len(self.buffer) > 1048576:
                    raise ConnectionError('Oversized stream record; reconnecting')
        except (OSError, ValueError) as error:
            self.close()
            self.status = str(error)
            self.retry = now + 1
        while self.frames and self.frames[0] < now - 2:
            self.frames.popleft()

    def rate(self):
        if len(self.frames) < 2:
            return 0.0
        return (len(self.frames) - 1) / max(0.001, time.monotonic() - self.frames[0])


def pose_lines(item):
    """Show raw local position and rotation rows; no Euler conventions to guess."""
    m = item.get('pose')
    if not isinstance(m, list) or len(m) != 16 or not all(
            isinstance(x, (float, int)) and math.isfinite(x) for x in m):
        return ['  Pose unavailable', '  R --', '    --', '    --']
    return [f'  XYZ m  {m[3]:+8.3f} {m[7]:+8.3f} {m[11]:+8.3f}',
            *[('  R ' if row == 0 else '    ') + ' '.join(
                f'{m[row*4+col]:+8.3f}' for col in range(3)) for row in range(3)]]


def number(value):
    return float(value) if isinstance(value, (float, int)) and math.isfinite(value) else 0.0


def bar(value):
    value = number(value)
    count = round(max(0, min(1, value)) * 10)
    return '[' + '#' * count + '.' * (10-count) + f'] {value:.2f}'


def view(stream):
    exit_hint = 'Ctrl-C stops telemetry' if os.environ.get('QUEST_VIEWER_OWNS_TELEMETRY') == '1' else 'Ctrl-C exits viewer'
    lines = [(f'QUEST TELEMETRY | q / Esc detach | {exit_hint}', 'title')]
    sample = stream.sample
    age = time.monotonic() - stream.received
    stale = sample is not None and age > 1.0
    if sample is None:
        lines += [(stream.status, 'warn'),
                  (f'TCP {stream.address[0]}:{stream.address[1]} — retrying automatically', ''),
                  ('This viewer does not start or stop the XR app.', '')]
        return lines
    focus = sample.get('input_focused')
    state = 'STALE — last sample only' if stale else 'LIVE'
    lines.append((f'{state} | {stream.rate():5.1f} Hz | age {age:.2f}s | frame {sample.get("frame_seq", "?")}',
                  'warn' if stale else 'good'))
    lines.append((f'Focus: {"XR" if focus is True else "SYSTEM / TERMINAL" if focus is False else "unknown"}'
                  f' | space: {sample.get("reference_space", "?")} | invalid records: {stream.bad}',
                  'warn' if focus is False else ''))
    sync = sample.get('action_sync_result')
    sync_name = 'OK' if sync == 0 else 'NOT_FOCUSED' if sync == 8 else str(sync) if sync is not None else '?'
    lines.append((f'XR: {sample.get("xr_session_state", "?")} | actions: {sync_name}'
                  f' | poll {number(sample.get("input_poll_ms")):.2f} ms',
                  'warn' if sync is not None and sync != 0 else ''))
    intent = sample.get('control_intent')
    if isinstance(intent, dict):
        hands = intent.get('hands', {})
        for side in ('left', 'right'):
            c = hands.get(side, {}) if isinstance(hands, dict) else {}
            if not isinstance(c, dict):
                continue
            state = 'STALE' if stale else 'INVALID' if not c.get('valid') else 'RELEASE GRIP' if c.get('needs_release') else 'ENGAGED' if c.get('engaged') else 'HOLD JOINTS'
            delta = c.get('delta_position', [0, 0, 0])
            if not isinstance(delta, list) or len(delta) != 3:
                delta = [0, 0, 0]
            xyz = ' '.join(f'{number(v):+.3f}' for v in delta)
            lines.append((f'{side.upper()} clutch {state} #{c.get("engagement_id", "?")} | dXYZ {xyz} m',
                          'good' if state == 'ENGAGED' else 'dim'))
    demo = sample.get('demo')
    if isinstance(demo, dict) and demo.get('enabled'):
        lines.append((f'LOCAL KINEMATIC DEMO | {demo.get("base_frame", "?")} base | Y resets robot', 'title'))
    lines.append((stream.notice, ''))
    for name, item in [('HEAD', sample['head']), ('LEFT', sample['hands'].get('left', {})),
                       ('RIGHT', sample['hands'].get('right', {}))]:
        if not isinstance(item, dict):
            item = {}
        tracked = bool(item.get('position_tracked') and item.get('orientation_tracked'))
        active = '' if name == 'HEAD' else (' active' if item.get('active') else ' inactive')
        lines.append((f'{name}{active} | {"tracked" if tracked else "untracked"}', 'title'))
        lines.extend((line, 'dim' if stale else '') for line in pose_lines(item))
        if name != 'HEAD':
            lines.append((f'  Trigger {bar(item.get("trigger"))}  Grip {bar(item.get("grip"))}', ''))
            stick = item.get('joystick', [0, 0])
            if not isinstance(stick, list) or len(stick) != 2:
                stick = [0, 0]
            buttons = item.get('buttons', {})
            pressed = ','.join(str(k) for k, v in buttons.items() if v) if isinstance(buttons, dict) else ''
            lines.append((f'  Stick {number(stick[0]):+.2f} {number(stick[1]):+.2f} | buttons: {pressed or "none"}', ''))
    lines.append(('R rows: local-from-device rotation; XYZ in OpenXR LOCAL, +Y up.', 'dim'))
    return lines


def run(screen, stream):
    try:
        curses.curs_set(0)
    except curses.error:
        pass
    screen.nodelay(True)
    styles = {'title': curses.A_BOLD, 'warn': curses.A_BOLD, 'good': 0, 'dim': curses.A_DIM, '': 0}
    if curses.has_colors():
        curses.start_color()
        curses.use_default_colors()
        for index, color in enumerate([curses.COLOR_CYAN, curses.COLOR_YELLOW, curses.COLOR_GREEN], 1):
            curses.init_pair(index, color, -1)
        styles.update(title=curses.color_pair(1) | curses.A_BOLD,
                      warn=curses.color_pair(2) | curses.A_BOLD, good=curses.color_pair(3))
    offset = 0
    while True:
        stream.tick()
        lines = view(stream)
        height, width = screen.getmaxyx()
        offset = min(offset, max(0, len(lines) - max(1, height-1)))
        screen.erase()
        for row, (text, style) in enumerate(lines[offset:offset+max(0, height-1)]):
            try:
                screen.addnstr(row, 0, text, max(0, width-1), styles[style])
            except curses.error:
                pass
        if height > 1 and height-1 < len(lines):
            try:
                screen.addnstr(height-1, 0, 'Up/Down scroll | resize terminal for all poses | q exits', max(0, width-1), curses.A_DIM)
            except curses.error:
                pass
        screen.refresh()
        key = screen.getch()
        if key in (ord('q'), ord('Q'), 27):
            return
        if key in (curses.KEY_DOWN, ord('j')):
            offset += 1
        elif key in (curses.KEY_UP, ord('k')):
            offset = max(0, offset-1)
        time.sleep(0.1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=9391)
    parser.add_argument('--focus', action='store_true', help='Resume existing local XR activity and exit')
    args = parser.parse_args()
    if args.focus:
        ok, message = focus_xr()
        print(message)
        raise SystemExit(0 if ok else 1)
    stream = Stream(args.host, args.port)
    try:
        curses.wrapper(run, stream)
    except KeyboardInterrupt:
        # The supervising launcher uses 130 to stop its native telemetry app.
        # Standalone q watch has no supervisor and remains read-only.
        raise SystemExit(130)
    finally:
        stream.close()


if __name__ == '__main__':
    main()
