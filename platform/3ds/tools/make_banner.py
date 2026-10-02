#!/usr/bin/env python3
# (AI-assisted)
# Makes the CIA's HOME Menu banner from your own copy of the game:
#   banner.png: the title screen logo model (lit with its vertex colors and envmap shine) and the
#               "the Precursor Legacy" sprite, 256 x 128 on a transparent background
#   banner.wav: the power cell jingle ("cell-prize"), cut to the HOME Menu's 3 seconds
#   icon.png:   with --icon IMAGE: a 48 x 48 crop of any picture (--icon-crop x0,y0,x1,y1)
# into platform/3ds/cia/local/ (ignored by git: game assets are never committed), where
# make_cia.sh and the .3dsx build pick them up.
#
# Needs the decompiler's exports: in decompiler/config/jak1/jak1_config.jsonc set
#   "rip_levels": true, "save_texture_pngs": true, "rip_streamed_audio": true
# and run the extraction (task extract). Then:
#   python3 platform/3ds/tools/make_banner.py [--decomp-out decompiler_out] [--out DIR]
# Needs numpy and Pillow (python3 -m pip install numpy pillow).
import argparse
import base64
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
ICON_SIZE = 48
SUPERSAMPLE = 4
MAX_SECONDS = 2.9  # the HOME Menu plays at most 3 s
ENV_GAIN = 0.3  # envmap shine; the game's fade isn't in the export, 0.3 looks like the title screen
SUBTITLE_W = 150  # "the Precursor Legacy", drawn larger than in the model so it reads at 256 x 128

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


def texture(gltf, binary, tex_index):
    """RGBA float image of a glTF texture (embedded as a data: URI or a buffer view), or None."""
    if tex_index is None:
        return None
    img = gltf["images"][gltf["textures"][tex_index]["source"]]
    if img.get("uri", "").startswith("data:"):
        blob = base64.b64decode(img["uri"].split(",", 1)[1])
    elif "bufferView" in img:
        view = gltf["bufferViews"][img["bufferView"]]
        start = view.get("byteOffset", 0)
        blob = binary[start : start + view["byteLength"]]
    else:
        return None
    return np.asarray(Image.open(io.BytesIO(blob)).convert("RGBA"), dtype=np.float64) / 255.0


def primitives(gltf, binary):
    """The mesh primitives in their bind pose (the logo's rest pose is how the title shows it)."""
    out = []
    for node in gltf["nodes"]:
        if "mesh" not in node:
            continue
        for prim in gltf["meshes"][node["mesh"]]["primitives"]:
            if prim.get("mode", 4) != 4:
                continue
            attrs = prim["attributes"]
            pos = read_accessor(gltf, binary, attrs["POSITION"])[:, :3]
            n = len(pos)
            nrm = (read_accessor(gltf, binary, attrs["NORMAL"])[:, :3] if "NORMAL" in attrs
                   else np.tile([0.0, 0.0, 1.0], (n, 1)))
            col = (read_accessor(gltf, binary, attrs["COLOR_0"]) if "COLOR_0" in attrs
                   else np.full((n, 4), 0.5))
            if col.shape[1] == 3:
                col = np.c_[col, np.ones(n)]
            uv = (read_accessor(gltf, binary, attrs["TEXCOORD_0"])[:, :2] if "TEXCOORD_0" in attrs
                  else np.zeros((n, 2)))
            idx = (read_accessor(gltf, binary, prim["indices"]).astype(np.int64).reshape(-1, 3)
                   if "indices" in prim else np.arange(n).reshape(-1, 3))
            mat = gltf["materials"][prim["material"]] if "material" in prim else {}
            pbr = mat.get("pbrMetallicRoughness", {})
            env = (mat.get("extensions", {}).get("KHR_materials_specular", {})
                   .get("specularColorTexture", {}).get("index"))
            out.append(dict(name=mat.get("name"), pos=pos, nrm=nrm, col=col, uv=uv, idx=idx,
                            tex=texture(gltf, binary, pbr.get("baseColorTexture", {}).get("index")),
                            env=texture(gltf, binary, env),
                            blend=mat.get("alphaMode") in ("MASK", "BLEND")))
    return out


def sample(tex, u, v, wrap):
    """Bilinear texture lookup."""
    th, tw = tex.shape[:2]
    if wrap:
        u, v = u % 1.0, v % 1.0
    x, y = u * tw - 0.5, v * th - 0.5
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = (x - x0)[..., None], (y - y0)[..., None]

    def at(xx, yy):
        if wrap:
            return tex[yy % th, xx % tw]
        return tex[np.clip(yy, 0, th - 1), np.clip(xx, 0, tw - 1)]

    return (at(x0, y0) * (1 - fx) * (1 - fy) + at(x0 + 1, y0) * fx * (1 - fy)
            + at(x0, y0 + 1) * (1 - fx) * fy + at(x0 + 1, y0 + 1) * fx * fy)


# ---------------------------------------------------------------------------------------------
# rendering: orthographic, straight on (the logo faces +z)


def render_model(glb_path, width, height, skip=(), margin=0.03):
    gltf, binary = load_glb(glb_path)
    prims = [p for p in primitives(gltf, binary) if p["name"] not in skip]
    if not prims:
        raise ValueError(f"{glb_path}: no triangles")
    used = np.concatenate([p["pos"][np.unique(p["idx"])] for p in prims])
    lo, hi = used.min(0), used.max(0)
    ext, center = hi - lo, (lo + hi) / 2
    W, H = width * SUPERSAMPLE, height * SUPERSAMPLE
    scale = min(W * (1 - 2 * margin) / ext[0], H * (1 - 2 * margin) / ext[1])
    color = np.zeros((H, W, 4))
    zbuf = np.full((H, W), -np.inf)
    # opaque parts first, then the alpha tested / blended ones (outline, sprites) over them
    for p in sorted(prims, key=lambda p: p["blend"]):
        sx = W / 2 + (p["pos"][:, 0] - center[0]) * scale
        sy = H / 2 - (p["pos"][:, 1] - center[1]) * scale
        sz = p["pos"][:, 2]  # bigger = closer
        for t in p["idx"]:
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
            bary = np.stack([w0, w1, 1 - w0 - w1], -1)
            inside = (bary >= 0).all(-1)
            if not inside.any():
                continue
            pz = bary @ z
            rgba = (bary @ p["col"][t]) * 2.0  # PS2 vertex colors: 0x80 = 1.0
            if p["tex"] is not None:
                uv = bary @ p["uv"][t]
                rgba = rgba * sample(p["tex"], uv[..., 0], uv[..., 1], wrap=not p["blend"])
                rgba[..., 3] *= 2.0  # PS2 texture alpha: 0x80 = opaque
            if p["env"] is not None:
                # sphere-mapped shine from the view space normal, added like emerc
                n = bary @ p["nrm"][t]
                n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-9)
                e = sample(p["env"], n[..., 0] * 0.5 + 0.5, -n[..., 1] * 0.5 + 0.5, wrap=False)
                rgba[..., :3] += e[..., :3] * 2.0 * ENV_GAIN
            a = np.clip(rgba[..., 3], 0, 1)
            rz = zbuf[y0 : y1 + 1, x0 : x1 + 1]
            rc = color[y0 : y1 + 1, x0 : x1 + 1]
            src = np.clip(rgba[..., :3], 0, 1)
            if p["blend"]:
                write = inside & (a > 0.02) & (pz >= rz - 1e-3)
                aa = np.where(write, a, 0.0)[..., None]
                rc[..., :3] = src * aa + rc[..., :3] * (1 - aa)
                rc[..., 3] = aa[..., 0] + rc[..., 3] * (1 - aa[..., 0])
            else:
                write = inside & (pz > rz)
                rz[write] = pz[write]
                rc[write] = np.c_[src[write], np.ones(write.sum())]
    img = Image.fromarray((np.clip(color, 0, 1) * 255).astype(np.uint8), "RGBA")
    return img.resize((width, height), Image.LANCZOS)


def make_banner(model, subtitle, out_png):
    """The logo model without its small subtitle quad, and the subtitle sprite larger below it."""
    W, H = BANNER_W, BANNER_H
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    sub = None
    if subtitle:
        sub = Image.open(subtitle).convert("RGBA")
        alpha = np.clip(np.asarray(sub)[..., 3].astype(np.float64) * 2, 0, 255)  # 0x80 = opaque
        sub.putalpha(Image.fromarray(alpha.astype(np.uint8)))
        sub = sub.crop(sub.getbbox())
        sub = sub.resize((SUBTITLE_W, round(sub.height * SUBTITLE_W / sub.width)), Image.LANCZOS)
    logo_h = H - (sub.height - 4 if sub else 0)
    skip = ("precursor_legacy", "trademark") if sub else ()
    logo = render_model(model, W, logo_h, skip=skip)
    out.alpha_composite(logo, (0, 2 if sub else 0))
    if sub:
        out.alpha_composite(sub, ((W - SUBTITLE_W) // 2 + 30, H - sub.height - 3))
    out.save(out_png)
    covered = (np.asarray(out)[..., 3] > 0).mean()
    print(f"banner image: {out_png} (from {model}, {covered * 100:.0f}% covered)")
    if covered < 0.02:
        print("  warning: almost nothing was drawn; check the image", file=sys.stderr)


def make_icon(image, crop, out_png):
    im = Image.open(image).convert("RGB")
    if crop:
        im = im.crop(tuple(int(v) for v in crop.split(",")))
    if im.width != im.height:
        print(f"  note: the icon crop is {im.width} x {im.height}, not square", file=sys.stderr)
    im.resize((ICON_SIZE, ICON_SIZE), Image.LANCZOS).save(out_png)
    print(f"icon: {out_png} (from {image})")


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


def find_one(patterns, what, hint, required=True):
    for p in patterns:
        hits = sorted(glob.glob(p, recursive=True))
        if hits:
            return hits[0]
    if required:
        sys.exit(f"no {what} found ({', '.join(patterns)}).\n{hint}")
    print(f"  note: no {what} found ({hint})", file=sys.stderr)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--decomp-out", default=os.path.join(ROOT, "decompiler_out"),
                    help="the decompiler's output folder (searched in its jak1* subfolders)")
    ap.add_argument("--out", default=os.path.join(ROOT, "platform", "3ds", "cia", "local"))
    ap.add_argument("--model", help="a .glb to render instead of the title logo")
    ap.add_argument("--sound", help="a .wav to use instead of cell-prize")
    ap.add_argument("--icon", help="a picture to make the 48 x 48 HOME Menu icon from")
    ap.add_argument("--icon-crop", help="x0,y0,x1,y1: the square of --icon to use")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    d = os.path.join(args.decomp_out, "jak1*")
    hint = ('Set "rip_levels", "save_texture_pngs" and "rip_streamed_audio" to true in '
            "decompiler/config/jak1/jak1_config.jsonc and extract again.")
    model = args.model or find_one(
        [os.path.join(d, "levels", "title", "logo-english-lod0.glb"),
         os.path.join(d, "levels", "**", "logo-english-lod0.glb")], "title logo model", hint)
    subtitle = find_one([os.path.join(d, "textures", "**", "precursor_legacy.png")],
                        "subtitle sprite (precursor_legacy.png)", hint, required=False)
    sound = args.sound or find_one(
        [os.path.join(d, "audio", "sfx", "COMMON", "cell-prize.wav"),
         os.path.join(d, "audio", "sfx", "**", "cell-prize.wav")],
        "power cell jingle (cell-prize.wav)", hint)
    make_banner(model, subtitle, os.path.join(args.out, "banner.png"))
    make_jingle(sound, os.path.join(args.out, "banner.wav"))
    if args.icon:
        make_icon(args.icon, args.icon_crop, os.path.join(args.out, "icon.png"))
    print("now: platform/3ds/tools/make_cia.sh (it uses the files in", args.out + ")")


if __name__ == "__main__":
    main()
