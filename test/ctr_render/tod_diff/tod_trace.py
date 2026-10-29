#!/usr/bin/env python3
# (AI-assisted) tod_diff: random time of day weights, one line per frame: frame, then per palette
# r g b alpha (64 = 1.0): small steps, jumps (flicker), one channel alone, alpha apart from rgb.
#   tod_trace.py <seed> <frames>
import random
import sys

seed, frames = int(sys.argv[1]), int(sys.argv[2])
random.seed(seed)
w = [[random.choice([0, 0, 32, 64, 128]) for c in range(4)] for p in range(8)]
for f in range(frames):
    for p in range(8):
        r = random.random()
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
