"""Headless edge-case suite for the Denoiser's temporal stage. Run with nuke -ti.
Env: SUITE_DIR (work folder), SUITE_DEVICES ("CPU" or "CPU,CUDA")."""
import math
import os
import sys
import traceback

import nuke

WORK = os.environ["SUITE_DIR"].replace("\\", "/")
DEVICES = os.environ.get("SUITE_DEVICES", "CPU").split(",")
WIDTH, HEIGHT = 160, 120
FIRST, LAST = 1, 10
SPEED = 4  # pixels per frame, +x
results = []


def check(name, condition, detail=""):
    results.append(bool(condition))
    print("[%s] %s %s" % ("PASS" if condition else "FAIL", name, detail))


# --- 1. A noisy multi-layer EXR sequence and its clean reference ------------------
fmt = nuke.addFormat("%d %d 1 suite_format" % (WIDTH, HEIGHT))
nuke.root()["format"].setValue("suite_format")
nuke.root()["first_frame"].setValue(FIRST)
nuke.root()["last_frame"].setValue(LAST)
nuke.Layer("albedo", ["albedo.red", "albedo.green", "albedo.blue"])
nuke.Layer("motion", ["motion.u", "motion.v"])
nuke.Layer("wild", ["wild.u", "wild.v"])

checker = nuke.nodes.CheckerBoard2(format="suite_format", boxsize=12)
moving = nuke.nodes.Transform(inputs=[checker])
moving["translate"].setExpression("%d*frame" % SPEED, 0)
clean = nuke.nodes.Blur(inputs=[moving], size=1)

noise = nuke.nodes.Expression(inputs=[clean])
for index in range(3):
    noise["expr%d" % index].setValue("(random(x*0.731, y*1.37, frame*7.31 + %d) - 0.5) * 0.5" % index)
noisy = nuke.nodes.Merge2(inputs=[clean, noise], operation="plus")
with_albedo = nuke.nodes.Copy(inputs=[noisy, clean])
for index, colour in enumerate(("red", "green", "blue")):
    with_albedo["from%d" % index].setValue("rgba." + colour)
    with_albedo["to%d" % index].setValue("albedo." + colour)
vectors = nuke.nodes.Expression(inputs=[with_albedo])
vectors["channel0"].setValue("motion.u")
vectors["expr0"].setValue(str(SPEED))
vectors["channel1"].setValue("motion.v")
vectors["expr1"].setValue("0")
vectors["channel2"].setValue("wild.u")
vectors["expr2"].setValue("(random(x, y, frame) - 0.5) * 1e7")
vectors["channel3"].setValue("wild.v")
vectors["expr3"].setValue("1e30")

seq_path = WORK + "/noisy.####.exr"
clean_path = WORK + "/clean.####.exr"
write_noisy = nuke.nodes.Write(inputs=[vectors], file=seq_path, file_type="exr", channels="all")
write_clean = nuke.nodes.Write(inputs=[clean], file=clean_path, file_type="exr")
nuke.execute(write_noisy, FIRST, LAST)
nuke.execute(write_clean, FIRST, LAST)
check("sequence written", os.path.exists(WORK + "/noisy.0005.exr"))

read = nuke.nodes.Read(file=seq_path, first=FIRST, last=LAST, origfirst=FIRST, origlast=LAST)
read.forceValidate()
reference = nuke.nodes.Read(file=clean_path, first=FIRST, last=LAST)
reference.forceValidate()
# Nuke maps motion.u/v onto its built-in forward.u/v channels.
check("layers present", "forward.u" in read.channels() and "albedo.red" in read.channels(),
      str(sorted(set(c.split(".")[0] for c in read.channels()))))


# --- helpers -------------------------------------------------------------------------
def make_denoiser(source, **knobs):
    node = nuke.nodes.Denoiser(inputs=[source])
    node["albedo_layer"].setValue("albedo")
    for key, value in knobs.items():
        node[key].setValue(value)
    return node


def pixels(node, frame, step=2):
    out = []
    for y in range(0, HEIGHT, step):
        for x in range(0, WIDTH, step):
            for channel in ("rgba.red", "rgba.green", "rgba.blue"):
                out.append(node.sample(channel, x + 0.5, y + 0.5, frame=frame))
    return out


def rmse(node, frame):
    got = pixels(node, frame)
    want = pixels(reference, frame)
    return math.sqrt(sum((a - b) ** 2 for a, b in zip(got, want)) / len(got))


def finite(node, frame):
    return all(math.isfinite(value) for value in pixels(node, frame, step=4))


# --- 2. Quality: spatial beats noisy, temporal beats spatial ---------------------------
mid = 5
noisy_rmse = rmse(read, mid)
spatial = make_denoiser(read)
spatial_rmse = rmse(spatial, mid)
check("spatial beats noisy", spatial_rmse < noisy_rmse, "%.4f < %.4f" % (spatial_rmse, noisy_rmse))

best_sign = None
for flip_label, scale in (("+mv", 1.0), ("-mv", -1.0)):
    node = make_denoiser(read, temporal=True, motion_layer="motion", temporal_radius=2)
    if scale < 0:
        # mv_scale's range is 0..2, so the opposite sign is tested with a negated vector layer.
        flipped = nuke.nodes.Expression(inputs=[read])
        flipped["channel0"].setValue("motion.u")
        flipped["expr0"].setValue("-motion.u")
        node.setInput(0, flipped)
    value = rmse(node, mid)
    print("[INFO] temporal radius 2 %s rmse %.4f" % (flip_label, value))
    if best_sign is None or value < best_sign[1]:
        best_sign = (flip_label, value)
check("temporal (best vector sign) beats spatial", best_sign[1] < spatial_rmse,
      "%s %.4f < %.4f" % (best_sign[0], best_sign[1], spatial_rmse))
check("forward vectors are the right sign", best_sign[0] == "+mv")

for radius in (1, 2, 3):
    node = make_denoiser(read, temporal=True, motion_layer="motion", temporal_radius=radius)
    value = rmse(node, mid)
    check("radius %d beats spatial" % radius, value < spatial_rmse, "%.4f" % value)

# --- 3. Sequence ends: neighbours outside the range ---------------------------------------
edge = make_denoiser(read, temporal=True, motion_layer="motion", temporal_radius=3)
for frame in (FIRST, LAST):
    value = rmse(edge, frame)
    check("frame %d radius 3 finite and not worse than noisy" % frame,
          finite(edge, frame) and value < rmse(read, frame), "%.4f" % value)
worse = []
for frame in range(FIRST, LAST + 1):
    temporal_value = rmse(edge, frame)
    spatial_value = rmse(spatial, frame)
    print("[INFO] frame %d spatial %.4f temporal r3 %.4f" % (frame, spatial_value, temporal_value))
    if temporal_value > spatial_value:
        worse.append(frame)
check("temporal r3 no worse than spatial on every frame", not worse, "worse on %s" % worse)
for before in ("black", "bounce", "loop"):
    read["before"].setValue(before)
    read["after"].setValue(before)
    check("read before/after=%s frame 1 finite" % before, finite(edge, FIRST))
read["before"].setValue("hold")
read["after"].setValue("hold")

# --- 4. Stateless: the order frames are rendered in does not change pixels --------------
order_node = make_denoiser(read, temporal=True, motion_layer="motion", temporal_radius=2)
out_path = WORK + "/order_%s.####.exr"
for label, frames in (("forward", [4, 5, 6]), ("backward", [8, 6, 5, 4])):
    write = nuke.nodes.Write(inputs=[order_node], file=out_path % label, file_type="exr")
    for frame in frames:
        nuke.execute(write, frame, frame)
    nuke.delete(write)
same = True
for frame in (4, 5, 6):
    forward = nuke.nodes.Read(file=out_path % "forward", first=4, last=6)
    backward = nuke.nodes.Read(file=out_path % "backward", first=4, last=8)
    same = same and pixels(forward, frame, 3) == pixels(backward, frame, 3)
check("render order does not change pixels", same)

# --- 5. Inactive and degenerate paths -----------------------------------------------------
no_motion = make_denoiser(read, temporal=True, temporal_radius=2)
no_motion["motion_layer"].setValue("none")
check("temporal without a motion layer equals spatial", pixels(no_motion, mid) == pixels(spatial, mid))
zero_strength = make_denoiser(read, temporal=True, motion_layer="motion", temporal_radius=2, temporal_strength=0.0)
check("strength 0 equals spatial", pixels(zero_strength, mid) == pixels(spatial, mid))
wild = make_denoiser(read, temporal=True, motion_layer="wild", temporal_radius=3)
check("huge and 1e30 vectors stay finite", finite(wild, mid))
zero_scale = make_denoiser(read, temporal=True, motion_layer="motion", temporal_radius=2, mv_scale=0.0)
check("vector scale 0 stays finite", finite(zero_scale, mid))
radius_over = make_denoiser(read, temporal=True, motion_layer="motion")
radius_over["temporal_radius"].setValue(99)
check("radius 99 is clamped and renders", finite(radius_over, mid))

# --- 6. Coordinate spaces: an upstream bbox larger than the format -------------------------
grown = nuke.nodes.AdjBBox(inputs=[read], numpixels=40)
big = make_denoiser(grown, temporal=True, motion_layer="motion", temporal_radius=2)
write = nuke.nodes.Write(inputs=[big], file=WORK + "/bbox.####.exr", file_type="exr")
try:
    nuke.execute(write, FIRST, LAST)
    check("expanded bbox, full range renders", True)
except Exception as error:
    check("expanded bbox, full range renders", False, str(error))
nuke.delete(write)

# --- 7. Devices and concurrency ------------------------------------------------------------
for device in DEVICES:
    node = make_denoiser(read, temporal=True, motion_layer="motion", temporal_radius=3, device=device)
    value = rmse(node, mid)
    check("%s radius 3 beats spatial" % device, value < spatial_rmse,
          "%.4f, knob=%s" % (value, node["device"].value()))
    writes = []
    for index in range(3):
        other = make_denoiser(read, temporal=True, motion_layer="motion", temporal_radius=2, device=device)
        writes.append(nuke.nodes.Write(inputs=[other], file=WORK + "/multi_%s_%d.####.exr" % (device, index),
                                       file_type="exr"))
    try:
        nuke.executeMultiple(writes, ((FIRST, LAST, 1),))
        check("%s three Denoisers, range render at once" % device, True)
    except Exception as error:
        check("%s three Denoisers, range render at once" % device, False, str(error))

passed = sum(results)
print("[SUITE] %d/%d passed" % (passed, len(results)))
sys.exit(0 if passed == len(results) else 1)
