#!/usr/bin/env python3
"""Convert icon.png to icon.ico with multiple sizes for Windows."""
import sys
from pathlib import Path
from PIL import Image

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.png> <output.ico>")
        sys.exit(1)

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])

    if not src.exists():
        print(f"Error: {src} not found")
        sys.exit(1)

    img = Image.open(src).convert("RGBA")

    # Crop to square (centered) — trim the longer dimension
    w, h = img.size
    if w != h:
        side = min(w, h)
        left = (w - side) // 2
        top = (h - side) // 2
        img = img.crop((left, top, left + side, top + side))

    # Generate each size explicitly with high-quality resampling
    sizes = [256, 128, 64, 48, 32, 24, 16]
    frames = []
    for s in sizes:
        resized = img.resize((s, s), Image.LANCZOS)
        frames.append(resized)

    # Save — first frame is the "main" image, rest go via append_images
    frames[0].save(dst, format="ICO", append_images=frames[1:])
    print(f"Generated {dst} from {src} ({len(frames)} sizes: {', '.join(str(s) for s in sizes)})")

if __name__ == "__main__":
    main()
