"""Generate the three camera-normal mirror-ball material probes.

Requires numpy, Pillow, and ImageMagick. Run from any working directory.
The image is indexed by the projected camera-space normal: uv = normal.xy/2 + 1/2.
The lighting panels are evaluated on the reflected view direction, so the
circle is a genuine sphere-map light probe rather than a radial gradient.
"""

from pathlib import Path
import subprocess
import tempfile

import numpy as np
from PIL import Image


SIZE = 512
ROOT = Path(__file__).resolve().parent / "textures"


def panel(direction, centre, width, height, softness):
    x = np.abs(direction[..., 0] - centre[0]) / width
    y = np.abs(direction[..., 1] - centre[1]) / height
    return np.exp(-softness * (np.maximum(x - 1.0, 0.0) ** 2 + np.maximum(y - 1.0, 0.0) ** 2))


def make_probe(name, base, key, fill, rim):
    axis = (np.arange(SIZE, dtype=np.float32) + 0.5) * (2.0 / SIZE) - 1.0
    x, y = np.meshgrid(axis, -axis)
    radius_squared = x * x + y * y
    z = np.sqrt(np.maximum(1.0 - radius_squared, 0.0))
    reflected = np.stack((2.0 * x * z, 2.0 * y * z, 2.0 * z * z - 1.0), axis=-1)

    # Broad softboxes and thin strip lights give the probe a studio reflection.
    lighting = np.broadcast_to(np.array(base, dtype=np.float32), reflected.shape).copy()
    lighting += panel(reflected, (-0.42, 0.43), 0.20, 0.45, 18.0)[..., None] * key
    lighting += panel(reflected, (0.58, 0.23), 0.075, 0.60, 35.0)[..., None] * fill
    lighting += panel(reflected, (0.03, -0.72), 0.65, 0.055, 28.0)[..., None] * rim
    lighting += (0.22 * np.maximum(reflected[..., 1], 0.0))[..., None] * key
    lighting *= (0.55 + 0.45 * z)[..., None]

    rgba = np.zeros((SIZE, SIZE, 4), dtype=np.uint8)
    rgba[..., :3] = np.uint8(np.clip(lighting, 0.0, 1.0) * 255.0)
    rgba[..., 3] = np.uint8(np.clip((1.0 - radius_squared) * 90.0, 0.0, 1.0) * 255.0)

    ROOT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        png = Path(temporary) / f"{name}.png"
        Image.fromarray(rgba, "RGBA").save(png)
        subprocess.run(
            ["magick", str(png), "-compress", "none", str(ROOT / f"{name}.tga")],
            check=True,
        )


if __name__ == "__main__":
    make_probe("probe_chrome", (0.08, 0.13, 0.21), (0.85, 0.91, 1.0), (0.42, 0.68, 0.90), (0.30, 0.46, 0.65))
    make_probe("probe_pearl", (0.20, 0.20, 0.24), (0.95, 0.89, 0.88), (0.52, 0.69, 0.85), (0.62, 0.45, 0.62))
    make_probe("probe_amber", (0.19, 0.09, 0.035), (1.0, 0.71, 0.32), (0.85, 0.33, 0.11), (0.68, 0.35, 0.12))
