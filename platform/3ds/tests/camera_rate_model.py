#!/usr/bin/env python3
"""Camera smoothing per second: the PS2 at 60 Hz, the 3DS at 30 fps before and after the
time-ratio scaling (goal_src/jak1/engine/camera, cam-steps / cam-keep / cam-part / cam-step).
Each case runs the per-frame rule with a fixed target and prints the state at 0.2 s, 0.5 s, 1 s.
Run: python3 platform/3ds/tests/camera_rate_model.py"""
import math

DEG = 65536 / 360.0
TIMES = (0.2, 0.5, 1.0)


def run(step, x0, frames_per_s, r, times=TIMES):
    """step(x, r) -> x for one frame of time-ratio r"""
    out, x, f = [], x0, 0
    for t in times:
        while f < round(t * frames_per_s):
            x = step(x, r)
            f += 1
        out.append(x)
    return out


def table(name, ps2, new, old, fmt="{:9.2f}"):
    worst = max(abs(a - b) for a, b in zip(ps2, new))
    print(f"{name}")
    for label, v in (("PS2 60 Hz", ps2), ("3DS new", new), ("3DS old", old)):
        print(f"  {label:10s}" + "".join(fmt.format(x) for x in v))
    return worst


def steps(r):
    return max(1, int(r + 0.5))


def keep(k, r):
    return k ** steps(r)


def part(p, r):
    return 1 - (1 - p) ** steps(r)


def compare(name, rule_new, rule_old, x0, scale=1.0, fmt="{:9.2f}"):
    ps2 = [v * scale for v in run(rule_old, x0, 60, 1)]
    new = [v * scale for v in run(rule_new, x0, 30, 2)]
    old = [v * scale for v in run(rule_old, x0, 30, 2)]
    w = table(name, ps2, new, old, fmt)
    rel = w / max(1e-9, abs(x0 * scale))
    print(f"  new vs PS2: max {w:.4g} ({rel * 100:.2f}% of the start)\n")
    return rel


worst = 0.0

# fov seek: 10 % of the way per frame, at most 1 degree (182.04) per frame
def fov_rule(fov, r, n=1):
    for _ in range(n):
        fov += max(-182.04445, min(182.04445, 0.1 * (11650.845 - fov)))
    return fov
worst = max(worst, compare("fov (deg), 95 -> 64 deg", lambda x, r: fov_rule(x, r, steps(r)), fov_rule, 17294.223, 1 / DEG))

# line-of-sight swing: matrix-from-two-vectors-max-angle-partial! (cos' = 1 + (cos - 1) p, at most 2.3 deg)
def los_once(a, cap=418.7022 / DEG, p=0.5):
    c = 1 + (math.cos(math.radians(a)) - 1) * p
    turn = min(math.degrees(math.acos(max(-1, min(1, c)))), cap)
    return max(0.0, a - turn)
worst = max(worst, compare("LOS swing: angle left (deg), from 20 deg",
                           lambda a, r: [a := los_once(a) for _ in range(steps(r))][-1], lambda a, r: los_once(a), 20.0))

# pull-in while blocked: 204.8 units (0.05 m) per frame
worst = max(worst, compare("LOS pull-in: distance (m), from 12 m", lambda d, r: d - 204.8 * r, lambda d, r: d - 204.8,
                           12 * 4096, 1 / 4096))

# hidden timer: 600 frames before the camera repositions
for label, fps, inc in (("PS2 60 Hz", 60, 1), ("3DS new", 30, 2), ("3DS old", 30, 1)):
    print(f"hidden timer: {label} repositions after {600 / inc / fps:.1f} s")
print()

# camera-master: vertical offset kept 90 % per frame on the ground, 50 % in the air
worst = max(worst, compare("ground pitch lag: offset left (m), from 1 m", lambda x, r: x * keep(0.9, r), lambda x, r: x * 0.9, 1.0, fmt="{:9.4f}"))
worst = max(worst, compare("air pitch lag: offset left (m), from 1 m", lambda x, r: x * keep(0.5, r), lambda x, r: x * 0.5, 1.0, fmt="{:9.5f}"))
worst = max(worst, compare("air target drift: offset left, 1/20 per frame", lambda x, r: x * (1 - part(0.05, r)), lambda x, r: x * 0.95, 1.0, fmt="{:9.4f}"))

# pitch-off: p = k (p + d), d = Jak's move per frame (6 m/s = 409.6 per PS2 frame)
def pitch_new(p, r, k=0.5, v=409.6):
    kn = keep(k, r)
    return kn * p + (1 - kn) * k / (1 - k) * (v * r) / steps(r)
worst = max(worst, compare("pitch-off at 6 m/s (units, PS2 steady = 409.6)", pitch_new, lambda p, r: 0.5 * (p + 409.6 * r), 0.0))

# follow-off: vector-seek-3d-smooth! 5 % per frame (max step scaled by seconds-per-frame)
def seek_old(x, r):
    return x - min(0.05 * x, 20480.0 * r / 60)
def seek_new(x, r):
    return x - min(part(0.05, r) * x, 20480.0 * r / 60)
worst = max(worst, compare("follow-off: offset left (m), from 2 m", seek_new, seek_old, 2 * 4096, 1 / 4096, "{:9.4f}"))

# cam-float-seeker (string-min/max: accel 40.96, max-vel 409.6, max-partial 0.9), step of 0.3 m
def seeker(state, dt):
    v, x = state
    e = 1228.8 - x
    v += e * 40.96 * dt
    lim = min(0.9 * abs(e), 409.6)
    if abs(v) > lim:
        v *= lim / abs(v)
    return (v, x + v * dt)
for label, fps, rule in (("PS2 60 Hz", 60, lambda s: seeker(s, 1.0)), ("3DS new", 30, lambda s: seeker(seeker(s, 1.0), 1.0)),
                         ("3DS old", 30, lambda s: seeker(s, 2.0))):
    s, xs = (0.0, 0.0), []
    for f in range(fps // 2):
        s = rule(s)
        xs.append(s[1] / 4096)
    print(f"seeker step 0 -> 0.3 m: {label:10s} peak {max(xs):.3f} m, at 0.5 s {xs[-1]:.3f} m")
print()

# slave-matrix-blend-2: a quarter of the angle per frame (Naughty Dog's 30 fps: r/4)
worst = max(worst, compare("rotation blend: angle left (deg), from 30 deg", lambda a, r: a * (1 - part(0.25, r)),
                           lambda a, r: a * (1 - 0.25 * r), 30.0))

# cam-decel: velocity kept 90 % per frame; the start velocity is one frame's move (2x at 30 fps)
for label, fps, k, v0 in (("PS2 60 Hz", 60, 0.9, 100.0), ("3DS new", 30, 0.81, 200.0), ("3DS old", 30, 0.9, 200.0)):
    v, x = v0, 0.0
    for _ in range(fps):
        v *= k
        x += v
    print(f"cam-decel glide: {label:10s} {x:7.1f} units in 1 s")
print()

# turn-around history: 16 entries, one per frame
for label, fps, n in (("PS2 60 Hz", 60, 16), ("3DS new", 30, 8), ("3DS old", 30, 16)):
    print(f"turn-around window: {label:10s} {n / fps:.3f} s")
print(f"\nworst new vs PS2 over the cases with a fixed target: {worst * 100:.2f}% of the start value")
