#!/usr/bin/env python3
# (AI-assisted)
# Makes the CIA's HOME Menu banner from your own copy of the game:
#   banner.png: the title screen logo model, rendered at 256 x 128 with a transparent background
#   banner.wav: the power cell jingle ("cell-prize"), cut to the HOME Menu's 3 seconds
# into platform/3ds/cia/local/ (ignored by git: game assets are never committed), where
# make_cia.sh picks them up.
#
# Needs the decompiler's exports: in decompiler/config/jak1/jak1_config.jsonc set
#   "rip_levels": true, "rip_streamed_audio": true
# and run the extraction (task extract). Then:
#   python3 platform/3ds/tools/make_banner.py [--decomp-out decompiler_out/jak1] [--out DIR]
# Needs numpy and Pillow (python3 -m pip install numpy pillow).
import argparse
import glob
import io
import json
import os
import struct
import sys
import wave

try:
    import numpy as np
    from PIL import Image
except ImportError:
    sys.exit("make_banner.py needs numpy and Pillow: python3 -m pip install numpy pillow")

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
BANNER_W, BANNER_H = 256, 128
SUPERSAMPLE = 4
MAX_SECONDS = 2.9  # the HOME Menu plays at most 3 s

# ---------------------------------------------------------------------------------------------
# GLB reading


def load_glb(path):
    data = open(path, "rb").read()
    magic, _version, _length = struct.unpack_from("<4sII", data, 0)
    if magic != b"glTF":
        raise ValueError(f"{path}: not a .glb file")
    off = 12
    gltf, binary = None, b""
    while off < len(data):
        clen, ctype = struct.unpack_from("<II", data, off)
        chunk = data[off + 8 : off + 8 + clen]
        if ctype == 0x4E4F534A:  # JSON
            gltf = json.loads(chunk)
        elif ctype == 0x004E4942:  # BIN
            binary = chunk
        off += 8 + clen
    return gltf, binary


COMPONENT = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32,
             5126: np.float32}
COUNT = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def read_accessor(gltf, binary, index):
    acc = gltf["accessors"][index]
    view = gltf["bufferViews"][acc["bufferView"]]
    dtype = np.dtype(COMPONENT[acc["componentType"]])
    n = COUNT[acc["type"]]
    count = acc["count"]
    start = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = view.get("byteStride", 0) or dtype.itemsize * n
    raw = np.frombuffer(binary, dtype=np.uint8, count=stride * (count - 1) + dtype.itemsize * n,
                        offset=start)
    rows = np.lib.stride_tricks.as_strided(raw, shape=(count, dtype.itemsize * n),
                                           strides=(stride, 1))
    out = np.ascontiguousarray(rows).view(dtype).reshape(count, n).astype(np.float64)
    if acc.get("normalized"):
        out /= float(np.iinfo(dtype).max)
    return out


def node_matrix(node):
    if "matrix" in node:
        return np.array(node["matrix"], dtype=np.float64).reshape(4, 4).T
    t = np.array(node.get("translation", [0, 0, 0]), dtype=np.float64)
    x, y, z, w = node.get("rotation", [0, 0, 0, 1])
    s = np.array(node.get("scale", [1, 1, 1]), dtype=np.float64)
    r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                  [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                  [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    m = np.eye(4)
    m[:3, :3] = r * s
    m[:3, 3] = t
    return m


def load_image(gltf, binary, image_index):
    img = gltf["images"][image_index]
    if "bufferView" in img:
        view = gltf["bufferViews"][img["bufferView"]]
        start = view.get("byteOffset", 0)
        blob = binary[start : start + view["byteLength"]]
        return np.asarray(Image.open(io.BytesIO(blob)).convert("RGBA"), dtype=np.float64) / 255.0
    return None


def collect_triangles(gltf, binary):
    """[(positions (n,3), uvs (n,2), indices (m,3), texture or None)] in model space."""
    images = {}
    out = []
    nodes = gltf.get("nodes", [])
    children = {c for n in nodes for c in n.get("children", [])}
    roots = [i for i in range(len(nodes)) if i not in children]

    def visit(i, parent):
        node = nodes[i]
        world = parent @ node_matrix(node)
        if "mesh" in node:
            # a skinned mesh in its bind pose is already in place (the node transform is ignored)
            m = np.eye(4) if "skin" in node else world
            for prim in gltf["meshes"][node["mesh"]]["primitives"]:
                if prim.get("mode", 4) != 4:
                    continue
                attrs = prim["attributes"]
                pos = read_accessor(gltf, binary, attrs["POSITION"])[:, :3]
                pos = (np.c_[pos, np.ones(len(pos))] @ m.T)[:, :3]
                uv = (read_accessor(gltf, binary, attrs["TEXCOORD_0"])[:, :2]
                      if "TEXCOORD_0" in attrs else np.zeros((len(pos), 2)))
                if "indices" in prim:
                    idx = read_accessor(gltf, binary, prim["indices"]).astype(np.int64).reshape(-1, 3)
                else:
                    idx = np.arange(len(pos)).reshape(-1, 3)
                tex = None
                if "material" in prim:
                    pbr = gltf["materials"][prim["material"]].get("pbrMetallicRoughness", {})
                    t = pbr.get("baseColorTexture")
                    if t is not None:
                        src = gltf["textures"][t["index"]].get("source")
                        if src is not None:
                            if src not in images:
                                images[src] = load_image(gltf, binary, src)
                            tex = images[src]
                out.append((pos, uv, idx, tex))
        for c in node.get("children", []):
            visit(c, world)

    for r in roots:
        visit(r, np.eye(4))
    return out


# ---------------------------------------------------------------------------------------------
# rendering: orthographic, from the side the logo's faces point to


def render_logo(glb_path, out_png):
    gltf, binary = load_glb(glb_path)
    tris = collect_triangles(gltf, binary)
    if not tris:
        raise ValueError(f"{glb_path}: no triangles")
    allpos = np.concatenate([p for p, _, _, _ in tris])
    lo, hi = allpos.min(0), allpos.max(0)
    ext = hi - lo
    depth_axis = int(np.argmin(ext))  # a logo is flat: view along its thinnest extent
    others = [a for a in range(3) if a != depth_axis]
    # up: y if it's one of the two, else z; right: the other one
    up_axis = 1 if 1 in others else 2
    right_axis = [a for a in others if a != up_axis][0]
    # front: the side most of the triangle area faces
    facing = 0.0
    for pos, _, idx, _ in tris:
        a, b, c = pos[idx[:, 0]], pos[idx[:, 1]], pos[idx[:, 2]]
        facing += np.cross(b - a, c - a)[:, depth_axis].sum()
    view_sign = 1.0 if facing >= 0 else -1.0  # we look from +depth towards -depth when facing > 0

    W, H = BANNER_W * SUPERSAMPLE, BANNER_H * SUPERSAMPLE
    margin = 0.06
    scale = min(W * (1 - 2 * margin) / max(ext[right_axis], 1e-9),
                H * (1 - 2 * margin) / max(ext[up_axis], 1e-9))
    center = (lo + hi) / 2
    # seen from the front, "right" flips when we look from the negative side
    right_sign = 1.0 if view_sign > 0 else -1.0
    if depth_axis == 1:  # looking down/up along y: keep the usual handedness
        right_sign = -right_sign

    color = np.zeros((H, W, 4))
    zbuf = np.full((H, W), -np.inf)
    for pos, uv, idx, tex in tris:
        sx = W / 2 + right_sign * (pos[:, right_axis] - center[right_axis]) * scale
        sy = H / 2 - (pos[:, up_axis] - center[up_axis]) * scale
        sz = view_sign * pos[:, depth_axis]  # bigger = closer to the viewer
        for t in idx:
            x, y, z = sx[t], sy[t], sz[t]
            area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0])
            if abs(area) < 1e-12:
                continue
            x0, x1 = max(int(np.floor(x.min())), 0), min(int(np.ceil(x.max())), W - 1)
            y0, y1 = max(int(np.floor(y.min())), 0), min(int(np.ceil(y.max())), H - 1)
            if x0 > x1 or y0 > y1:
                continue
            px, py = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            w0 = ((x[1] - px) * (y[2] - py) - (x[2] - px) * (y[1] - py)) / area
            w1 = ((x[2] - px) * (y[0] - py) - (x[0] - px) * (y[2] - py)) / area
            w2 = 1 - w0 - w1
            inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            if not inside.any():
                continue
            pz = w0 * z[0] + w1 * z[1] + w2 * z[2]
            if tex is not None:
                u = w0 * uv[t[0], 0] + w1 * uv[t[1], 0] + w2 * uv[t[2], 0]
                v = w0 * uv[t[0], 1] + w1 * uv[t[1], 1] + w2 * uv[t[2], 1]
                th, tw = tex.shape[:2]
                tx = (np.floor((u % 1.0) * tw).astype(int)) % tw
                ty = (np.floor((v % 1.0) * th).astype(int)) % th
                rgba = tex[ty, tx]
            else:
                rgba = np.broadcast_to(np.array([0.8, 0.8, 0.8, 1.0]), px.shape + (4,))
            region_z = zbuf[y0:y1 + 1, x0:x1 + 1]
            # cut-outs: (nearly) transparent texels don't hide what's behind
            write = inside & (pz > region_z) & (rgba[..., 3] > 0.1)
            region_z[write] = pz[write]
            region_c = color[y0:y1 + 1, x0:x1 + 1]
            region_c[write] = rgba[write]
    # PS2 texture alpha is 0x80 = opaque: anything visible is opaque in the banner
    color[..., 3] = np.where(color[..., 3] > 0.1, 1.0, 0.0)
    img = Image.fromarray((np.clip(color, 0, 1) * 255).astype(np.uint8), "RGBA")
    img = img.resize((BANNER_W, BANNER_H), Image.LANCZOS)
    img.save(out_png)
    covered = (np.asarray(img)[..., 3] > 0).mean()
    print(f"banner image: {out_png} (from {glb_path}, {covered * 100:.0f}% covered)")
    if covered < 0.02:
        print("  warning: almost nothing was drawn; check the image", file=sys.stderr)


# ---------------------------------------------------------------------------------------------
# audio


def make_jingle(wav_in, wav_out):
    with wave.open(wav_in, "rb") as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        frames = w.readframes(n)
    if width != 2:
        sys.exit(f"{wav_in}: expected 16-bit samples, got {8 * width}-bit")
    s = np.frombuffer(frames, dtype="<i2").reshape(-1, ch).astype(np.float64)
    keep = min(len(s), int(MAX_SECONDS * rate))
    s = s[:keep]
    fade = min(int(0.4 * rate), keep)
    if keep < n and fade > 0:  # cut short: fade out the end
        s[-fade:] *= np.linspace(1.0, 0.0, fade)[:, None]
    with wave.open(wav_out, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(np.clip(s, -32768, 32767).astype("<i2").tobytes())
    print(f"banner jingle: {wav_out} (from {wav_in}, {keep / rate:.2f} s, {rate} Hz, {ch} ch)")


def find_one(patterns, what, hint):
    for p in patterns:
        hits = sorted(glob.glob(p, recursive=True))
        if hits:
            return hits[0]
    sys.exit(f"no {what} found ({', '.join(patterns)}).\n{hint}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--decomp-out", default=os.path.join(ROOT, "decompiler_out", "jak1"))
    ap.add_argument("--out", default=os.path.join(ROOT, "platform", "3ds", "cia", "local"))
    ap.add_argument("--model", help="a .glb to render instead of the title logo")
    ap.add_argument("--sound", help="a .wav to use instead of cell-prize")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    d = args.decomp_out
    model = args.model or find_one(
        [os.path.join(d, "levels", "title", "logo-english-lod0.glb"),
         os.path.join(d, "levels", "**", "logo-english-lod0.glb"),
         os.path.join(d, "levels", "**", "logo-lod0.glb")],
        "title logo model",
        'Set "rip_levels": true in decompiler/config/jak1/jak1_config.jsonc and extract again.')
    sound = args.sound or find_one(
        [os.path.join(d, "audio", "sfx", "**", "cell-prize.wav")],
        "power cell jingle (cell-prize.wav)",
        'Set "rip_streamed_audio": true in decompiler/config/jak1/jak1_config.jsonc and extract '
        "again.")
    render_logo(model, os.path.join(args.out, "banner.png"))
    make_jingle(sound, os.path.join(args.out, "banner.wav"))
    print("now: platform/3ds/tools/make_cia.sh (it uses the banner in", args.out + ")")


if __name__ == "__main__":
    main()
