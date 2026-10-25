"""Generate the site's responsive WebP derivatives; retain the original PNGs.

Run from the repository root with Pillow installed. No build-time or browser
dependency is introduced: the generated files are checked in with the site.
"""

from pathlib import Path

from PIL import Image, features


ASSETS = Path(__file__).resolve().parents[2] / "website" / "assets"
NAMES = ("dashboard-grid", "dashboard", "games-list", "theme_charcoal", "theme_original")
WIDTHS = (480, 800, 1200)


def main():
    if not features.check("webp"):
        raise SystemExit("Pillow must include WebP support.")

    original_total = 0
    largest_total = 0
    for name in NAMES:
        source = ASSETS / f"{name}.png"
        original_total += source.stat().st_size
        with Image.open(source) as image:
            if image.size != (1200, 800):
                raise SystemExit(f"{source}: update HTML dimensions/srcsets for {image.size} first.")
            # Flatten transparency against the site's background, not white.
            background = Image.new("RGBA", image.size, "#111218")
            background.alpha_composite(image.convert("RGBA"))
            rgb = background.convert("RGB")
            for width in WIDTHS:
                output = ASSETS / f"{name}-{width}.webp"
                resized = rgb.resize((width, round(width * 2 / 3)), Image.Resampling.LANCZOS)
                resized.save(output, "WEBP", quality=90, method=6)
                if width == 1200:
                    largest_total += output.stat().st_size
                print(f"{output.name}: {output.stat().st_size:,} bytes")
    saving = 100 * (1 - largest_total / original_total)
    print(f"1200px set: {original_total:,} PNG bytes → {largest_total:,} WebP bytes ({saving:.1f}% smaller)")


if __name__ == "__main__":
    main()
