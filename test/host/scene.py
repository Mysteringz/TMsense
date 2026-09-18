"""Physically based synthetic scenes for a ceiling-mounted MLX90640 (110 x 75 deg).

Geometry: an f-theta lens looking straight down from `mount_m` metres. Pixel
(u, v) looks along angles ((u+.5-16)/16 * 55 deg, (v+.5-12)/12 * 37.5 deg).
Each pixel is supersampled 6x6; each ray hits a person (a disc at head and
shoulder height) or else the floor, and the pixel reads the mean temperature
of what its rays hit -- so a person narrower than a pixel produces the partial,
diluted contrast a real sensor sees.

Units: metres on the floor, origin under the sensor, x along the sensor's u
axis, y along v.
"""
import math
import numpy as np

W, H = 32, 24
HALF_FOV_X = math.radians(55.0)
HALF_FOV_Y = math.radians(37.5)
SS = 6

_off = (np.arange(SS) + 0.5) / SS
_u = (np.arange(W)[:, None] + _off[None, :]).reshape(-1)          # W*SS
_v = (np.arange(H)[:, None] + _off[None, :]).reshape(-1)          # H*SS
TAN_X = np.tan((_u - W / 2) / (W / 2) * HALF_FOV_X)               # per sub-column
TAN_Y = np.tan((_v - H / 2) / (H / 2) * HALF_FOV_Y)               # per sub-row


def pixel_of(x_m, y_m, mount_m, z_m=1.1):
    """Floor point (at height z) -> fractional pixel coordinates."""
    d = mount_m - z_m
    u = math.atan2(x_m, d) / HALF_FOV_X * (W / 2) + W / 2
    v = math.atan2(y_m, d) / HALF_FOV_Y * (H / 2) + H / 2
    return u, v


class Person:
    """Seated person seen from above: warm core (head) inside cooler shoulders."""
    def __init__(self, x, y, core_c=31.5, body_c=29.0, core_r=0.12, body_r=0.26, z=1.1):
        self.x, self.y, self.core_c, self.body_c = x, y, core_c, body_c
        self.core_r, self.body_r, self.z = core_r, body_r, z


class Blob:
    """Any warm disc: a laptop, a mug, a radiator."""
    def __init__(self, x, y, r, temp_c, z=0.75):
        self.x, self.y, self.r, self.t, self.z = x, y, r, temp_c, z


def render(mount_m, people=(), blobs=(), floor_c=23.0, rng=None, noise_c=0.18,
           gradient_c=1.5, offset_c=0.0):
    fx = mount_m * TAN_X[None, :]
    fy = mount_m * TAN_Y[:, None]
    # A room is not flat: a gentle gradient plus fixed furniture texture.
    temp = floor_c + offset_c + gradient_c * (fx / 8.0) + 0.4 * np.sin(fx * 1.3) * np.cos(fy * 1.1)
    temp = np.broadcast_to(temp, (H * SS, W * SS)).copy()
    for b in blobs:
        d = mount_m - b.z
        hit = (d * TAN_X[None, :] - b.x) ** 2 + (d * TAN_Y[:, None] - b.y) ** 2 <= b.r ** 2
        temp[hit] = b.t
    for p in people:
        d = mount_m - p.z
        r2 = (d * TAN_X[None, :] - p.x) ** 2 + (d * TAN_Y[:, None] - p.y) ** 2
        temp[r2 <= p.body_r ** 2] = p.body_c
        temp[r2 <= p.core_r ** 2] = p.core_c
    img = temp.reshape(H, SS, W, SS).mean(axis=(1, 3))
    if rng is not None:
        img = img + rng.normal(0.0, noise_c, img.shape)
    return img.astype(np.float32)
