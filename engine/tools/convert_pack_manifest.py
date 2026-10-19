#!/usr/bin/env python3
"""Convert an art-export manifest.json (CloudAtlas's file inventory) into the engine's
resources.json schema (docs/RESOURCE_PACKS.md).

Reads   <pack>/manifest.json (never modified)
Writes  <out>/resources.json (default: next to the manifest)

  python tools/convert_pack_manifest.py assets/packs/pack_64
  python tools/convert_pack_manifest.py <export>/pack_64 --out assets/packs/pack_64

Rules (the only art-specific knowledge lives here, not in C++):
  textures/blocks/*.png   -> "textures" (vertical strips with a .png.json sidecar -> "animations")
  textures/blocks/*.svg   -> "textures" (Litho SVG; a .svg.json sidecar strip -> "animations").
                             SVG entries were silently dropped before P3-3 (Appendix E L8).
  A manifest file entry carrying a "provenance" object has it passed through
  onto the generated entry ({"file": ..., "provenance": {...}}). The converter
  never INVENTS a stamp: provenance asserts litho-generated origin, which only
  the generating tool may claim.
  (textures/ui and textures/branding are not block textures and are skipped)
  textures/masks/*.png    -> "textures"; <block>_<role>_mask -> that block's masks[role],
                             <block>_mask -> masks["main"]
  textures/palettes/*.png -> "palettes"; a wire uses the palette of its own name, colour lamps
                             use PALETTE_FOR below
  manifest block/face/piece/lit/state -> "models" (base vs active layouts, wire pieces)
  *_slab bottom = slab top; grass bottom = dirt (EXTRA_FACES)
"""
import argparse
import json
import os
import sys

PALETTE_FOR = {"color_lamp_16": "cga16", "color_lamp_32": "vga32", "color_lamp_64": "vga64"}
EXTRA_FACES = {"grass": {"bottom": "dirt"}}
# Models the engine cannot express yet (art batch 3: facing-relative Redirector overlays and split
# fronts need an overlay pass; their textures still load). Until then these models are pinned.
PINNED_MODELS = {
    "redirector": {"faces": {"front": "redirector_front", "side": "redirector_side", "top": "redirector_top"},
                   "active": {"front": "redirector_front_on", "top": "redirector_top_on"}},
}


def classify(entry):
    """base | active | both | anim. "firing" = a one-shot strip shown while active; "on",
    "pressed" and "playing" = active; "off" and "empty" = base; anything else = both."""
    if "lit" in entry:
        return "active" if entry["lit"] else "base"
    state = entry.get("state", "")
    if state.startswith("firing"):
        return "anim"
    if state.startswith("on") or state in ("pressed", "playing"):
        return "active"
    if state.startswith("off") or state == "empty":
        return "base"
    return "both"


def stem(path):
    return os.path.splitext(os.path.basename(path))[0]


def convert(manifest, pack_dir):
    textures, animations, palettes, masks = {}, {}, {}, {}
    by_block = {}
    for f in manifest.get("files", []):
        path = f.get("path", "")
        if not (path.endswith(".png") or path.endswith(".svg")):
            continue
        name = stem(path)

        def entry():
            # Provenance pass-through (P3-3, Appendix E L8): keep a stamp the
            # export carries; never fabricate one.
            if isinstance(f.get("provenance"), dict):
                return {"file": path, "provenance": f["provenance"]}
            return path

        if path.startswith("textures/palettes/"):
            palettes[name] = entry()
            continue
        if path.startswith("textures/masks/"):
            textures[name] = entry()
            masks[name] = path
            continue
        if not path.startswith("textures/blocks/"):
            continue  # UI art is not part of the block texture array
        is_strip = f.get("height", 0) > f.get("width", 0) or os.path.exists(os.path.join(pack_dir, path + ".json"))
        if is_strip:
            anim = entry()
            animations[name] = anim if isinstance(anim, dict) else {"file": path}
        else:
            textures[name] = entry()
        if f.get("block"):
            by_block.setdefault(f["block"], []).append((f, name))

    models = {}
    for block, entries in sorted(by_block.items()):
        model = {}
        pieces = {e["piece"]: n for e, n in entries if e.get("piece")}
        if pieces:
            model["wire"] = dict(sorted(pieces.items()))
        faces = {}
        for e, n in entries:
            if e.get("piece"):
                continue
            faces.setdefault(e.get("face") or "all", {})[classify(e)] = n
        def ref(n):  # animation strips are referenced as {"animation": name}
            return {"animation": n} if n in animations else n

        base, active = {}, {}
        for face, cls in faces.items():
            b = cls.get("base") or cls.get("both") or cls.get("active")
            if b:
                base[face] = ref(b)
            if "anim" in cls:
                active[face] = ref(cls["anim"])
            elif "active" in cls and cls["active"] != b:
                active[face] = ref(cls["active"])
            elif "base" in cls and "both" in cls:
                active[face] = ref(cls["both"])
        if block.endswith("_slab") and "top" in base and "bottom" not in base:
            base["bottom"] = base["top"]
        for face, tex in EXTRA_FACES.get(block, {}).items():
            base.setdefault(face, tex)
        if base:
            model["faces"] = dict(sorted(base.items()))
        if active:
            model["active"] = dict(sorted(active.items()))
        palette = PALETTE_FOR.get(block) or (block if block in palettes else None)
        if palette:
            model["palette"] = palette
        roles = {}
        for m in masks:
            if m == block + "_mask":
                roles["main"] = m
            elif m.startswith(block + "_") and m.endswith("_mask"):
                roles[m[len(block) + 1:-len("_mask")]] = m
        if roles:
            model["masks"] = dict(sorted(roles.items()))
        models[block] = PINNED_MODELS.get(block, model)

    return {
        "format": "litholattice.resources",
        "version": 1,
        "generated_by": "tools/convert_pack_manifest.py from manifest.json (" + manifest.get("pack", "?") + ")",
        "tile_size": manifest.get("tile_size", 16),
        "textures": dict(sorted(textures.items())),
        "animations": dict(sorted(animations.items())),
        "palettes": dict(sorted(palettes.items())),
        "models": models,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pack", help="folder containing manifest.json")
    ap.add_argument("--out", help="folder for resources.json (default: the pack folder)")
    args = ap.parse_args()
    with open(os.path.join(args.pack, "manifest.json"), encoding="utf-8") as fh:
        manifest = json.load(fh)
    result = convert(manifest, args.pack)
    out_dir = args.out or args.pack
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "resources.json")
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(result, fh, indent=1)
        fh.write("\n")
    print("%s: %d textures, %d animations, %d palettes, %d models" %
          (out, len(result["textures"]), len(result["animations"]), len(result["palettes"]), len(result["models"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
