#!/usr/bin/env python3
# (AI-assisted) tod_diff: random time of day weights, one line per frame: frame, then per palette
# r g b alpha (64 = 1.0): small steps, jumps (flicker), one channel alone, alpha apart from rgb.
#   tod_trace.py <seed> <frames> [lights MASK | lightning]
# lights MASK: the palettes in MASK move every frame (lamps, lava glow), the others rarely
# lightning: Rock Village at 30 fps without day steps (palettes 0 and 1 fixed): mood.gc
# update-mood-lightning's flashes on palettes 2 and 3 (a *flash* table value per drawn frame, 90-240
# frames apart), update-mood-flames on 4-7 (two 60 Hz steps per drawn frame)
import math
import random
import sys

seed, frames = int(sys.argv[1]), int(sys.argv[2])
lights = int(sys.argv[4], 0) if len(sys.argv) > 4 and sys.argv[3] == 'lights' else None
random.seed(seed)
if len(sys.argv) > 3 and sys.argv[3] == 'lightning':
    flash = [[1.0, 0.0, 0.5, 1.0, 0.5, 0.0, 0.5, 0.35, 0.4, 0.35, 0.25, 0.1, 0.04],
             [1.0, 0.8, 0.0, 1.0, 0.5, 1.0, 0.4, 0.2, 0.1],
             [1.0, 0.9, 0.8, 0.7, 0.0, 0.0, 1.0, 0.0, 1.0, 0.5],
             [0.5, 0.0, 1.0, 0.9, 1.0, 0.8, 0.3, 0.0, 0.0, 0.5, 0.1, 0.5, 0.35],
             [1.0, 0.0, 1.0, 0.0, 1.0, 0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.5, 0.4, 0.3, 0.2,
              0.1],
             [1.0, 0.0, 1.0, 0.0] + [1.0 - 0.05 * k for k in range(17)] +
             [0.5 - 0.05 * k for k in range(10)],
             [1.0, 0.0, 1.0, 0.0, 0.5, 0.0, 0.5, 0.35, 0.0, 0.0, 1.0, 0.0, 0.2, 0.1],
             [1.0, 0.8, 0.3, 0.0, 0.6, 0.5, 0.4, 0.3, 0.2, 0.5, 0.4, 0.3, 0.2, 0.1]]
    w64 = lambda f: int(f * 4096) >> 6  # update-mood-itimes: vftoi12, >> 6
    day = random.randint(0, 64)
    gap, idx, tab, t = random.randint(10, 40), 0, None, 0
    fl = [0, 0, 0, 0]  # update-mood-flames' state: index, time, length, height
    for f in range(frames):
        w = [[day] * 4, [64 - day] * 4] + [[0] * 4 for _ in range(6)]
        v = 0.0
        if tab is not None:
            if t < len(tab):
                v, t = tab[t], t + 1
            else:
                tab, gap = None, random.randint(90, 240)
        else:
            gap -= 1
            if gap == 0:
                idx, tab, t = (idx + 1) % 2, random.choice(flash), 0
                v, t = tab[0], 1
        w[2 + idx] = [w64(v)] * 4
        for _ in range(2):
            times = [0.333] * 4
            if fl[1] >= fl[2]:
                fl = [int(random.uniform(0, 3.99)), 0, int(random.uniform(7, 15)),
                      int(random.uniform(0, 255))]
            else:
                times[fl[0]] = fl[3] * math.sin(math.pi * fl[1] / fl[2]) * 0.001953125 + 0.333
                fl[1] += 1
        for k in range(4):
            w[4 + k] = [w64(times[k])] * 4
        print(' '.join(map(str, [1000 + f] + [x for p in range(8) for x in w[p]])))
    sys.exit(0)
w = [[random.choice([0, 0, 32, 64, 128]) for c in range(4)] for p in range(8)]
for f in range(frames):
    for p in range(8):
        r = random.random()
        if lights is not None and not (lights >> p & 1):
            r = 1.0 if random.random() < 0.98 else 0.0
        elif lights is not None:
            r = min(r, 0.37)
        if r < 0.3:
            d = random.choice([-2, -1, 1, 2])
            for c in range(3):
                w[p][c] = max(0, min(255, w[p][c] + d))
            if random.random() < 0.7:
                w[p][3] = w[p][0]
        elif r < 0.35:
            v = random.choice([0, 16, 64, 100, 200, 255])
            w[p] = [v] * 4
        elif r < 0.38:
            c = random.randrange(4)
            w[p][c] = max(0, min(255, w[p][c] + random.randint(-9, 9)))
    print(' '.join(map(str, [1000 + f] + [x for p in range(8) for x in w[p]])))
