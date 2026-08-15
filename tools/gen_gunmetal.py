#!/usr/bin/env python3
"""Generate the Podslicer gunmetal-PEI banner texture.

Output: resources/images/gunmetal_pei.png -- 1024x256, horizontally tileable.
The look: dark gunmetal blue-grey ground, PEI powder-coat speckle at two grains,
a faint horizontal brushed anisotropy, sparse arctic-tinted glints, and a baked
specular sheen. Vertical structure is baked (banners stretch to height); the
horizontal axis must tile, which is why every layer is either horizontally
constant or seam-blended.

Deterministic: fixed seed, so the asset regenerates byte-stable.
"""
import numpy as np
from PIL import Image, ImageFilter
from pathlib import Path

W, H = 1024, 256
SEAM = 96  # columns crossfaded onto the start to make the noise tile in x

rng = np.random.default_rng(20260815)


def tileable(noise: np.ndarray) -> np.ndarray:
    """Crossfade the trailing SEAM columns into the leading ones."""
    out = noise[:, :W].copy()
    tail = noise[:, W:W + SEAM]
    t = np.linspace(0.0, 1.0, SEAM)[None, :]
    out[:, :SEAM] = out[:, :SEAM] * t + tail * (1.0 - t)
    return out


# -- ground: vertical gunmetal gradient (blue-grey, darker at both edges) -----
top, mid, bot = np.array([0x1A, 0x1E, 0x23]), np.array([0x23, 0x28, 0x2E]), np.array([0x17, 0x1B, 0x20])
y = np.linspace(0.0, 1.0, H)[:, None]
up, down = np.clip(y * 2, 0, 1), np.clip((y - 0.5) * 2, 0, 1)
ground = (top[None, None, :] * (1 - up[..., None])
          + mid[None, None, :] * (up * (1 - down))[..., None]
          + bot[None, None, :] * down[..., None]).astype(np.float64)
img = np.broadcast_to(ground, (H, W, 3)).copy()

# -- PEI powder-coat speckle: fine grain + coarser mottling -------------------
fine = tileable(rng.normal(0.0, 1.0, (H, W + SEAM)))
coarse = tileable(rng.normal(0.0, 1.0, (H // 4, W // 4 + SEAM)).repeat(4, 0).repeat(4, 1)[:, :W + SEAM])
coarse = np.asarray(Image.fromarray(((coarse - coarse.min()) / np.ptp(coarse) * 255).astype(np.uint8))
                    .filter(ImageFilter.GaussianBlur(2)), dtype=np.float64) / 255.0 - 0.5
img += fine[..., None] * 4.5 + coarse[..., None] * 7.0

# -- brushed anisotropy: horizontally smeared noise band ----------------------
brush = tileable(rng.normal(0.0, 1.0, (H, W + SEAM)))
brush = np.asarray(Image.fromarray(((brush - brush.min()) / np.ptp(brush) * 255).astype(np.uint8))
                   .filter(ImageFilter.BoxBlur((17, 0))), dtype=np.float64) / 255.0 - 0.5
img += brush[..., None] * 5.0

# -- sparse glints: PEI flecks catching light, arctic-tinted ------------------
fleck_src = tileable(rng.random((H, W + SEAM)))
flecks = (fleck_src > 0.9985).astype(np.float64)
bloom = np.asarray(Image.fromarray((flecks * 255).astype(np.uint8))
                   .filter(ImageFilter.GaussianBlur(1.1)), dtype=np.float64) / 255.0
arctic = np.array([0x8F, 0xD0, 0xEA], dtype=np.float64)
img += bloom[..., None] * arctic[None, None, :] * 0.55 + flecks[..., None] * 40.0

# -- baked specular sheen: soft band above centre, horizontally constant ------
sheen = np.exp(-((y - 0.30) ** 2) / (2 * 0.16 ** 2))          # broad key light
sheen += 0.35 * np.exp(-((y - 0.06) ** 2) / (2 * 0.03 ** 2))  # tight top-edge highlight
tint = np.array([0xB8, 0xD8, 0xEA], dtype=np.float64)          # cool specular colour
img += (sheen[..., None] * tint[None, None, :]) * 0.085
img[H - 2:, :, :] *= 0.82                                      # bottom shadow hairline

out = Path(__file__).resolve().parent.parent / "resources" / "images" / "gunmetal_pei.png"
Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(out)
print(f"wrote {out}")
