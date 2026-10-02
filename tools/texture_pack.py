#!/usr/bin/env python3
"""Builds a texture pack (higher resolution textures for the Xbox maps' bitmaps).

    python tools/texture_pack.py extract --map maps/bloodgulch.map [--map ...] --work work
    python tools/texture_pack.py upscale --work work [--scale 2]
    python tools/texture_pack.py pack --work work --out texture_pack

extract: decodes every 2D bitmap of the maps (as xbox_textures.c decodes it
    for the GPU) into work/src/<crc>.png, <crc> being the CRC-32 of the first
    mip level's bytes as the map holds them, which is also how the game finds
    a bitmap again (port/linux/src/texture_pack.c). Identical bitmaps of any
    number of maps share a file. work/index.json says what each one is and
    whether it is meant for upscaling; --stats prints what the maps hold.
upscale: runs Real-ESRGAN (realesrgan-ncnn-vulkan) over the ones meant for
    upscaling, colour and alpha separately, into work/up/<crc>.png.
pack: copies the upscaled textures into a folder the game reads
    (texture_pack/ in its data folder).

Needs Pillow and NumPy; upscale needs realesrgan-ncnn-vulkan (--esrgan).
"""

import argparse
import collections
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hud_assets import XboxMap, morton_order, decode_dxt  # noqa: E402

BITMAP_LINEAR_FLAG = 0x10
FORMATS = {0: "a8", 1: "y8", 2: "ay8", 3: "a8y8", 6: "r5g6b5", 8: "a1r5g5b5", 9: "a4r4g4b4",
           10: "x8r8g8b8", 11: "a8r8g8b8", 14: "dxt1", 15: "dxt3", 16: "dxt5", 17: "p8"}
BYTES_PER_TEXEL = {"a8": 1, "y8": 1, "ay8": 1, "a8y8": 2, "r5g6b5": 2, "a1r5g5b5": 2, "a4r4g4b4": 2,
                   "x8r8g8b8": 4, "a8r8g8b8": 4, "p8": 1}
USAGES = {0: "alpha_blend", 1: "default", 2: "height", 3: "detail", 4: "lightmap", 5: "vector"}
TYPES = {0: "2d", 1: "3d", 2: "cube"}

# names that say a bitmap holds data and not a picture: its channels are
# masks (specular, self-illumination, colour change) or vectors, which a
# network that invents detail would corrupt
DATA_WORDS = ("multipurpose", "_mp", " mp", "bump", "normal", "lightmap", "light map", "cube", "vector", "ramp",
              "gradient", "noise", "reflection")


def level0_size(width: int, height: int, kind: str) -> int:
    if kind.startswith("dxt"):
        return max(1, width // 4) * max(1, height // 4) * (8 if kind == "dxt1" else 16)
    return width * height * BYTES_PER_TEXEL[kind]


def expand(values: np.ndarray, bits: int) -> np.ndarray:
    return (values * 255 // ((1 << bits) - 1)).astype(np.uint8)


def decode(raw: bytes, width: int, height: int, kind: str, linear: bool) -> np.ndarray:
    """Level 0 as RGBA, texel for texel as xbox_textures.c's convert_texel."""
    if kind.startswith("dxt"):
        return decode_dxt(kind, raw, width, height)
    texel = BYTES_PER_TEXEL[kind]
    data = np.frombuffer(raw[:width * height * texel], np.uint8)
    order = np.arange(width * height).reshape(height, width) if linear else morton_order(width, height)
    pixels = data.reshape(-1, texel)[order]
    image = np.zeros((height, width, 4), np.uint8)
    if kind == "a8":
        image[..., :3], image[..., 3] = 255, pixels[..., 0]
    elif kind == "y8":
        image[..., :3], image[..., 3] = pixels[..., :1], 255
    elif kind == "ay8":
        image[...] = pixels[..., :1]
    elif kind == "a8y8":
        image[..., :3], image[..., 3] = pixels[..., :1], pixels[..., 1]
    elif kind in ("a8r8g8b8", "x8r8g8b8"):
        image[..., 0], image[..., 1], image[..., 2] = pixels[..., 2], pixels[..., 1], pixels[..., 0]
        image[..., 3] = pixels[..., 3] if kind == "a8r8g8b8" else 255
    else:
        value = pixels[..., 0].astype(np.uint16) | (pixels[..., 1].astype(np.uint16) << 8)
        if kind == "r5g6b5":
            image[..., 0], image[..., 1], image[..., 2] = (expand((value >> 11) & 31, 5),
                                                          expand((value >> 5) & 63, 6), expand(value & 31, 5))
            image[..., 3] = 255
        elif kind == "a1r5g5b5":
            image[..., 0], image[..., 1], image[..., 2] = (expand((value >> 10) & 31, 5),
                                                          expand((value >> 5) & 31, 5), expand(value & 31, 5))
            image[..., 3] = np.where(value & 0x8000, 255, 0)
        elif kind == "a4r4g4b4":
            image[..., 3], image[..., 0], image[..., 1], image[..., 2] = (expand(value >> 12, 4),
                                                                         expand((value >> 8) & 15, 4),
                                                                         expand((value >> 4) & 15, 4),
                                                                         expand(value & 15, 4))
        else:
            raise ValueError(kind)
    return image


def bitmaps_of(xbox_map: XboxMap):
    """Every (tag name, group usage, index, bitmap dict) of the map's bitmap tags."""
    for (group, name), address in xbox_map.tags.items():
        if group != "bitm":
            continue
        header = xbox_map.read(address, 0x6C)
        usage, = struct.unpack_from("<h", header, 4)
        count, blocks = struct.unpack_from("<II", header, 0x60)
        for index in range(count):
            block = xbox_map.read(blocks + index * 0x30, 0x30)
            width, height, depth, kind, form, flags = struct.unpack_from("<hhhhhH", block, 4)
            offset, size = struct.unpack_from("<ii", block, 0x18)
            yield name, usage, index, {"width": width, "height": height, "depth": depth, "type": kind,
                                       "format": form, "flags": flags, "pixels": xbox_map.data[offset:offset + size]}


def policy(name: str, usage: int, bitmap: dict) -> str:
    """'upscale', or why not."""
    if bitmap["type"] != 0:
        return "skip:" + TYPES.get(bitmap["type"], "type")
    if bitmap["format"] not in FORMATS or FORMATS[bitmap["format"]] == "p8":
        return "skip:format"
    if usage in (2, 4, 5):
        return "skip:" + USAGES[usage]
    lowered = name.lower()
    if any(word in lowered for word in DATA_WORDS):
        return "skip:name"
    if lowered.startswith(("ui\\", "effects\\", "rasterizer\\")):
        return "skip:hud/effect"
    if bitmap["width"] < 16 or bitmap["height"] < 16:
        return "skip:tiny"
    return "upscale"


def extract(arguments) -> None:
    work = Path(arguments.work)
    (work / "src").mkdir(parents=True, exist_ok=True)
    index_path = work / "index.json"
    index = json.loads(index_path.read_text()) if index_path.exists() else {}
    stats = collections.Counter()
    for map_path in arguments.map:
        xbox_map = XboxMap(Path(map_path))
        seen = 0
        for name, usage, number, bitmap in bitmaps_of(xbox_map):
            seen += 1
            if bitmap["format"] not in FORMATS or bitmap["width"] <= 0 or bitmap["height"] <= 0:
                stats["unknown format"] += 1
                continue
            kind = FORMATS[bitmap["format"]]
            raw = bitmap["pixels"][:level0_size(bitmap["width"], bitmap["height"], kind)]
            crc = "%08x" % zlib.crc32(raw)
            verdict = policy(name, usage, bitmap)
            stats[verdict] += 1
            entry = index.setdefault(crc, {"width": bitmap["width"], "height": bitmap["height"], "kind": kind,
                                           "usage": USAGES.get(usage, str(usage)), "verdict": verdict, "tags": []})
            entry["verdict"] = verdict
            label = "%s#%d" % (name, number)
            if label not in entry["tags"] and len(entry["tags"]) < 6:
                entry["tags"].append(label)
            path = work / "src" / (crc + ".png")
            if verdict == "upscale" and not path.exists() and not arguments.stats:
                Image.fromarray(decode(raw, bitmap["width"], bitmap["height"], kind,
                                       bool(bitmap["flags"] & BITMAP_LINEAR_FLAG)), "RGBA").save(path)
        print(Path(map_path).name, seen, "bitmaps")
    for verdict, count in sorted(stats.items()):
        print("  %-18s %d" % (verdict, count))
    unique = collections.Counter(entry["verdict"] for entry in index.values())
    print("unique so far:", dict(unique))
    if not arguments.stats:
        index_path.write_text(json.dumps(index, indent=1, sort_keys=True))


def dilate(rgb: np.ndarray, alpha: np.ndarray, rounds: int = 8) -> np.ndarray:
    """Colour for the texels with no alpha, copied outward from the ones that
    have some, so the network and the later resize do not drag the (arbitrary)
    colour a transparent texel holds into the edges of what is visible."""
    rgb = rgb.copy()
    known = alpha > 0
    for _ in range(rounds):
        if known.all():
            break
        total = np.zeros(rgb.shape, np.float32)
        count = np.zeros(known.shape, np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dx == 0 and dy == 0:
                    continue
                shifted_known = np.roll(np.roll(known, dy, 0), dx, 1)
                shifted_rgb = np.roll(np.roll(rgb, dy, 0), dx, 1)
                total += shifted_rgb * shifted_known[..., None]
                count += shifted_known
        fill = (~known) & (count > 0)
        rgb[fill] = (total[fill] / count[fill][:, None]).astype(np.uint8)
        known = known | fill
    return rgb


def factor_for(width: int, height: int, arguments) -> int:
    for factor in (4, 2):
        if factor <= arguments.scale and max(width, height) * factor <= arguments.max_side:
            return factor
    return 0


def result_error(source: Image.Image, result: Image.Image) -> float:
    """How far an upscaled result is from its source, 0-255: the result shrunk back
    to the source's size, its colour (where the source is visible) and its alpha
    compared. A real upscale of a texture is within a few levels of it; a result
    with tiles the upscaler failed to compute (the GPU lost the device and it
    wrote black) is nowhere near."""
    source = source.convert("RGBA")
    shrunk = np.asarray(result.convert("RGBA").resize(source.size, Image.BOX), np.float32)
    original = np.asarray(source, np.float32)
    visible = original[..., 3] > 16
    colour = np.abs(shrunk[..., :3] - original[..., :3])[visible].mean() if visible.any() else 0.0
    alpha = np.abs(shrunk[..., 3] - original[..., 3]).mean()
    return float(max(colour, alpha))


MAXIMUM_ERROR = 22.0


def upscale(arguments) -> None:
    work = Path(arguments.work)
    index = json.loads((work / "index.json").read_text())
    (work / "up").mkdir(exist_ok=True)
    todo = []
    for crc, entry in sorted(index.items()):
        if entry["verdict"] != "upscale" or (work / "up" / (crc + ".png")).exists():
            continue
        if not (work / "src" / (crc + ".png")).exists():
            continue
        if arguments.prefix and not any(tag.startswith(arguments.prefix) for tag in entry["tags"]):
            continue
        if factor_for(entry["width"], entry["height"], arguments):
            todo.append(crc)
    if arguments.limit:
        todo = todo[:arguments.limit]
    print(len(todo), "to upscale")
    failed = []
    for start in range(0, len(todo), arguments.batch):
        batch = todo[start:start + arguments.batch]
        with tempfile.TemporaryDirectory() as temporary:
            temporary = Path(temporary)
            (temporary / "in").mkdir()
            (temporary / "out").mkdir()
            alphas = {}
            for crc in batch:
                image = np.array(Image.open(work / "src" / (crc + ".png")).convert("RGBA"))
                alpha = image[..., 3]
                colour = dilate(image[..., :3], alpha) if (alpha < 255).any() else image[..., :3]
                Image.fromarray(colour, "RGB").save(temporary / "in" / (crc + "_c.png"))
                alphas[crc] = bool((alpha != alpha[0, 0]).any() or alpha[0, 0] != 255)
                if alphas[crc]:
                    Image.fromarray(np.repeat(alpha[..., None], 3, 2), "RGB").save(temporary / "in" / (crc + "_a.png"))
            subprocess.run([arguments.esrgan, "-i", temporary / "in", "-o", temporary / "out", "-n", arguments.model,
                            "-s", "4", "-t", str(arguments.tile), "-f", "png"], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for crc in batch:
                entry = index[crc]
                size = (entry["width"] * factor_for(entry["width"], entry["height"], arguments),
                        entry["height"] * factor_for(entry["width"], entry["height"], arguments))
                colour = Image.open(temporary / "out" / (crc + "_c.png")).convert("RGB").resize(size, Image.LANCZOS)
                if alphas[crc]:
                    alpha = Image.open(temporary / "out" / (crc + "_a.png")).convert("L").resize(size, Image.LANCZOS)
                else:
                    alpha = Image.new("L", size, 255)
                result = colour.convert("RGBA")
                result.putalpha(alpha)
                error = result_error(Image.open(work / "src" / (crc + ".png")), result)
                if error > MAXIMUM_ERROR:
                    failed.append(crc)
                    print("    %s rejected (error %.0f)" % (crc, error), flush=True)
                    continue
                result.save(work / "up" / (crc + ".png"))
        print("  %d/%d" % (min(start + arguments.batch, len(todo)), len(todo)), flush=True)
    # the ones the GPU failed on get smaller tiles, which it copes with
    for tile in (32, 16):
        if not failed:
            break
        retry, failed = failed, []
        print("retrying %d at tile %d" % (len(retry), tile), flush=True)
        for crc in retry:
            entry = index[crc]
            factor = factor_for(entry["width"], entry["height"], arguments)
            image = np.array(Image.open(work / "src" / (crc + ".png")).convert("RGBA"))
            alpha = image[..., 3]
            colour = dilate(image[..., :3], alpha) if (alpha < 255).any() else image[..., :3]
            with tempfile.TemporaryDirectory() as temporary:
                temporary = Path(temporary)
                (temporary / "in").mkdir()
                (temporary / "out").mkdir()
                Image.fromarray(colour, "RGB").save(temporary / "in" / (crc + "_c.png"))
                varies = bool((alpha != alpha[0, 0]).any() or alpha[0, 0] != 255)
                if varies:
                    Image.fromarray(np.repeat(alpha[..., None], 3, 2), "RGB").save(temporary / "in" / (crc + "_a.png"))
                subprocess.run([arguments.esrgan, "-i", temporary / "in", "-o", temporary / "out", "-n", arguments.model,
                                "-s", "4", "-t", str(tile), "-f", "png"], check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                size = (entry["width"] * factor, entry["height"] * factor)
                up_colour = Image.open(temporary / "out" / (crc + "_c.png")).convert("RGB").resize(size, Image.LANCZOS)
                up_alpha = (Image.open(temporary / "out" / (crc + "_a.png")).convert("L").resize(size, Image.LANCZOS)
                            if varies else Image.new("L", size, 255))
                result = up_colour.convert("RGBA")
                result.putalpha(up_alpha)
            if result_error(Image.open(work / "src" / (crc + ".png")), result) > MAXIMUM_ERROR:
                failed.append(crc)
            else:
                result.save(work / "up" / (crc + ".png"))
    for crc in failed:
        print("  gave up on", crc, index[crc]["tags"][0], "- left as the map has it")


def validate(arguments) -> None:
    work = Path(arguments.work)
    bad = []
    for path in sorted((work / "up").glob("*.png")):
        error = result_error(Image.open(work / "src" / path.name), Image.open(path))
        if error > MAXIMUM_ERROR:
            bad.append((path.stem, error))
            if arguments.delete:
                path.unlink()
    for crc, error in bad:
        print("  bad %s error %.0f" % (crc, error))
    print(len(bad), "bad results" + (" deleted" if arguments.delete else ""))


def pack(arguments) -> None:
    work, out = Path(arguments.work), Path(arguments.out)
    out.mkdir(parents=True, exist_ok=True)
    count = 0
    for path in sorted((work / "up").glob("*.png")):
        if arguments.prefix:
            index = json.loads((work / "index.json").read_text())
            if not any(tag.startswith(arguments.prefix) for tag in index[path.stem]["tags"]):
                continue
        shutil.copyfile(path, out / path.name)
        count += 1
    names = sorted(path.stem for path in out.glob("*.png"))
    (out / "index.txt").write_text("".join(name + "\n" for name in names))
    print(count, "textures in", out, "(index.txt lists", len(names), ")")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    one = commands.add_parser("extract")
    one.add_argument("--map", action="append", required=True)
    one.add_argument("--work", required=True)
    one.add_argument("--stats", action="store_true")
    one.set_defaults(run=extract)
    two = commands.add_parser("upscale")
    two.add_argument("--work", required=True)
    two.add_argument("--esrgan", default="realesrgan-ncnn-vulkan")
    two.add_argument("--model", default="realesrgan-x4plus")
    two.add_argument("--scale", type=int, default=2, choices=(2, 4))
    two.add_argument("--max-side", type=int, default=1024)
    two.add_argument("--tile", type=int, default=64)
    two.add_argument("--batch", type=int, default=24)
    two.add_argument("--limit", type=int, default=0)
    two.add_argument("--prefix", default="")
    two.set_defaults(run=upscale)
    five = commands.add_parser("validate")
    five.add_argument("--work", required=True)
    five.add_argument("--delete", action="store_true")
    five.set_defaults(run=validate)
    three = commands.add_parser("pack")
    three.add_argument("--work", required=True)
    three.add_argument("--out", required=True)
    three.add_argument("--prefix", default="")
    three.set_defaults(run=pack)
    arguments = parser.parse_args()
    arguments.run(arguments)


if __name__ == "__main__":
    main()
