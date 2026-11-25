#!/usr/bin/env python3
"""r275 follow mode: render the sooty showcase movie while the fire solver is still running.

usage: follow_render.py <run dir> <out dir> [options]      (standard library only)

Watches <run dir> (a `FireSequenceTest --fire-production-temporal-capstone 8 0.0625 <run dir>` run, live or
finished). For every new complete frame it
  1. derives the render frame (tools/fire/r272/chem_from_reaction.cpp: chem_CH/C2/CO2 rewritten from the local
     reaction rate, fraction 3.0e-6, exactly as the old mkseq.sh did) into <out>/seq/frames/,
  2. extends the manifest in <out>/seq/ to the frames derived so far,
  3. renders that one frame with tools/fire/r273/r273_sooty_showcase.RISEscene.in to <out>/fire_display*NNNN.*
     (same names as render_sooty_movie.py), and
  4. when the run is finished (production_capstone_summary sealed) and all 235 frames exist, encodes and verifies
     the movie exactly as render_sooty_movie.py does (r242_encode.swift / r242_verify.swift).
Restartable: state lives in <out>/follow_state.json and the images; frames whose derived file, manifest entry and
valid denoised PNG exist are skipped. Works on a live run and on a finished one.

Where the frames are. The solver publishes each frame as <run>/checkpoints/frame_NNNNN.vdb by atomic rename
(plus a sidecar) and then writes <run>/checkpoints/frame_NNNNN.time, whose second line is the VDB's SHA-256.
<run>/frames/ is only filled when the whole run ends. A frame is complete when its .time record parses and the
VDB hashes to the recorded digest; frames/ + frame_times.csv is the fallback for runs without checkpoints/.

Manifest. The render frames differ from the solver frames (chem rewrite), so the solver's manifest cannot be used
as is. If the run has a solver manifest -- sequence_manifest.partial.rise-fire.cbor (written by the capstone after
every frame, r275 and later) or the sealed sequence_manifest.rise-fire.cbor of a finished run -- its frames are
checked against the verified source digests and its frame list is re-pointed at the derived frames in Python
(canonical RISE-CBOR64-v1; the payload's sequence_id is recomputed; everything else, including the case record
and time map, is the solver's). Otherwise (older solver binary, e.g. the live run it was first started on) the
manifest is rebuilt with `FireSequenceTest --fire-production-manifest-rebuild`, as mkseq.sh did.

Frame k of the animation is scene time k/16 s (animation_options 0..14.625 s, 235 frames); the manifest maps it to
simulation time t0 + k/16 and to frame index k whatever length the manifest has, so frame k is the same image
(up to Monte Carlo noise) as frame k of one `renderanimation 0 14.625 235` pass. Each render process runs
`renderanimation <k>` for a batch of frames, with RISE_FIRE_SEQUENCE_PREFLIGHT=used so the renderer verifies only
the frames it actually uses (each frame is still hash-checked on load) instead of sweeping every listed frame.
CPU: the renderer gets --threads worker threads (default 8, via a one-line RISE_OPTIONS_FILE:
force_number_of_threads) and runs at nice 10, so a running solver keeps priority.

Control: touch <out>/assemble.request to get a partial movie of the contiguous frames rendered so far (the
follower keeps going); --assemble-now does that once and exits; --once renders what is ready and exits.
"""
import argparse, csv, hashlib, json, os, pathlib, re, shutil, signal, struct, subprocess, sys, time, datetime

ROOT = pathlib.Path(__file__).resolve().parents[3]
SCENE_TEMPLATE = ROOT / "tools/fire/r273/r273_sooty_showcase.RISEscene.in"
CHEM_SOURCE = ROOT / "tools/fire/r272/chem_from_reaction.cpp"
MAKE_DIR = ROOT / "build/make/rise"
TOTAL_FRAMES = 235
CADENCE_S = 0.0625
PARTIAL_MANIFEST = "sequence_manifest.partial.rise-fire.cbor"
FINAL_MANIFEST = "sequence_manifest.rise-fire.cbor"
SUMMARY_SEAL = "production_capstone_summary.txt.payload-v2.json"
# Environment the manifest rebuild needs to derive the same case record and stencil as the run (the mkseq.sh set).
REBUILD_ENV = {"RISE_FIRE_FLOOR_EMISSIVITY": "1", "RISE_FIRE_COMPACT_VISCOUS_STENCIL": "1",
               "RISE_FIRE_LIMITED_MOMENTUM_CARRIER": "1", "RISE_FIRE_VREMAN_ZONE": "below:16:0.2857",
               "RISE_FIRE_METHANE_SOOT_YIELD": "0.03"}


def log(message):
    print(datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"), message, flush=True)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def write_atomic(path, data):
    tmp = path.with_name(path.name + ".tmp")
    mode = "wb" if isinstance(data, bytes) else "w"
    with open(tmp, mode) as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


# ---------------------------------------------------------------- RISE-CBOR64-v1 (src/Library/Utilities/RISECBOR64.cpp)
class CMap(list):
    """Map as an ordered list of (key, value); encoding sorts by UTF-8 key bytes."""


def cbor_decode(data):
    pos = 0

    def head():
        nonlocal pos
        initial = data[pos]
        pos += 1
        major, add = initial >> 5, initial & 31
        if add < 24:
            return major, add
        width = {24: 1, 25: 2, 26: 4, 27: 8}.get(add)
        if width is None:
            raise ValueError("indefinite or reserved CBOR head")
        arg = int.from_bytes(data[pos:pos + width], "big")
        pos += width
        return major, arg

    def value():
        nonlocal pos
        initial = data[pos]
        if initial >> 5 == 7:
            pos += 1
            add = initial & 31
            if add in (20, 21):
                return add == 21
            if add == 22:
                return None
            if add == 27:
                (number,) = struct.unpack(">d", data[pos:pos + 8])
                pos += 8
                return number
            raise ValueError("unsupported simple value")
        major, arg = head()
        if major == 0:
            return arg
        if major == 1:
            return -1 - arg
        if major == 2:
            out = bytes(data[pos:pos + arg])
            pos += arg
            return out
        if major == 3:
            out = bytes(data[pos:pos + arg]).decode("utf-8")
            pos += arg
            return out
        if major == 4:
            return [value() for _ in range(arg)]
        if major == 5:
            members = CMap()
            for _ in range(arg):
                key = value()
                if not isinstance(key, str):
                    raise ValueError("map key is not text")
                members.append((key, value()))
            return members
        raise ValueError("unsupported CBOR major type")

    result = value()
    if pos != len(data):
        raise ValueError("trailing bytes after CBOR value")
    return result


def cbor_encode(v):
    def head(major, arg):
        base = major << 5
        if arg < 24:
            return bytes([base | arg])
        for add, width in ((24, 1), (25, 2), (26, 4), (27, 8)):
            if arg < (1 << (8 * width)):
                return bytes([base | add]) + arg.to_bytes(width, "big")
        raise ValueError("integer too large")

    if v is None:
        return b"\xf6"
    if v is True:
        return b"\xf5"
    if v is False:
        return b"\xf4"
    if isinstance(v, int):
        return head(0, v) if v >= 0 else head(1, -1 - v)
    if isinstance(v, float):
        return b"\xfb" + struct.pack(">d", 0.0 if v == 0.0 else v)
    if isinstance(v, str):
        raw = v.encode("utf-8")
        return head(3, len(raw)) + raw
    if isinstance(v, bytes):
        return head(2, len(v)) + v
    if isinstance(v, CMap):
        members = sorted(((k.encode("utf-8"), k, val) for k, val in v), key=lambda m: m[0])
        out = head(5, len(members))
        for raw, _, val in members:
            out += head(3, len(raw)) + raw + cbor_encode(val)
        return out
    if isinstance(v, list):
        return head(4, len(v)) + b"".join(cbor_encode(x) for x in v)
    raise TypeError(type(v))


def cmap_get(m, key):
    for k, val in m:
        if k == key:
            return val
    raise KeyError(key)


def cmap_set(m, key, value):
    for i, (k, _) in enumerate(m):
        if k == key:
            m[i] = (key, value)
            return
    raise KeyError(key)


def repoint_manifest(solver_manifest, frames):
    """solver manifest bytes + [(relative path, sha256)] for frames 0..K-1 -> canonical manifest bytes for those frames."""
    if cbor_encode(cbor_decode(solver_manifest)) != solver_manifest:
        raise ValueError("solver manifest does not round-trip through the canonical encoder")
    envelope = cbor_decode(solver_manifest)
    payload = cmap_get(envelope, "payload")
    old = cmap_get(payload, "frames")
    if len(frames) < 2 or len(frames) > len(old):
        raise ValueError("solver manifest lists %d frames, %d wanted" % (len(old), len(frames)))
    new = []
    for index, (path, digest) in enumerate(frames):
        if cmap_get(old[index], "index") != index:
            raise ValueError("solver manifest frame indices are not 0..N-1")
        new.append(CMap([("index", index), ("path", path), ("sha256", digest)]))
    cmap_set(payload, "frames", new)
    cmap_set(payload, "frame_count", len(new))
    cmap_set(payload, "last_frame_index", len(new) - 1)
    if cmap_get(payload, "first_frame_index") != 0:
        raise ValueError("solver manifest does not start at frame 0")
    sequence_id = hashlib.sha256(cbor_encode(payload)).hexdigest()
    return cbor_encode(CMap([("payload", payload), ("sequence_id", sequence_id)]))


def manifest_frame_digests(solver_manifest):
    payload = cmap_get(cbor_decode(solver_manifest), "payload")
    return [cmap_get(f, "sha256") for f in cmap_get(payload, "frames")]


# ---------------------------------------------------------------------------------------------------- tools
def build_chem_tool(tools):
    """Compile tools/fire/r272/chem_from_reaction.cpp with the repo's own compile/link flags (the r273 build_tool.py
    technique: take the FireSequenceTest compile and link lines from make -n and swap in this source)."""
    exe = tools / "chem_from_reaction"
    if exe.exists() and exe.stat().st_mtime >= CHEM_SOURCE.stat().st_mtime:
        return exe
    import shlex
    cmds = subprocess.check_output(["make", "-C", str(MAKE_DIR), "-n", "-B", "../../../bin/tests/FireSequenceTest"],
                                   text=True).splitlines()
    compile_ = next(x for x in cmds if x.startswith("c++ ") and " -c " in x and "tests/FireSequenceTest.cpp" in x)
    link = next(x for x in cmds if x.startswith("c++ ") and " -o ../../../bin/tests/FireSequenceTest " in x)
    obj = tools / "chem_from_reaction.o"
    compile_ = compile_.replace("../../../tests/FireSequenceTest.cpp", str(CHEM_SOURCE)).replace(
        "-o ../../../tests/FireSequenceTest.o", "-o " + str(obj))
    link = link.replace(" -o ../../../bin/tests/FireSequenceTest ", " -o " + str(exe) + " ").replace(
        " ../../../tests/FireSequenceTest.o ", " " + str(obj) + " ")
    if str(obj) not in compile_ or str(obj) not in link:
        raise RuntimeError("could not derive the chem_from_reaction build commands")
    script = tools / "build_chem_from_reaction.sh"
    script.write_text("set -e\ncd " + shlex.quote(str(MAKE_DIR)) + "\n" + compile_ + "\n" + link + "\n")
    with open(tools / "build_chem_from_reaction.log", "w") as out:
        subprocess.run(["sh", str(script)], stdout=out, stderr=subprocess.STDOUT, check=True)
    log("built " + str(exe))
    return exe


def snapshot_binary(src, dest, refresh):
    """Copy a repo-built binary into the output dir so a rebuild of the repo cannot change a long follow."""
    if refresh or not dest.exists():
        if not src.is_file():
            raise RuntimeError(str(src) + " is missing; build it first (make -C build/make/rise)")
        shutil.copy2(src, dest)
        log("tool %s sha256=%s" % (dest.name, sha256_file(dest)))
    return dest


# ---------------------------------------------------------------------------------------------------- follower
class Follower:
    IDLE = object()
    def __init__(self, args):
        self.args = args
        self.run = pathlib.Path(args.run_dir).resolve()
        self.out = pathlib.Path(args.out_dir).resolve()
        self.seq = self.out / "seq"
        self.tools = self.out / "tools"
        for d in (self.out, self.seq / "frames", self.tools, self.out / "logs"):
            d.mkdir(parents=True, exist_ok=True)
        self.state_path = self.out / "follow_state.json"
        self.state = {"derived": {}, "rendered": {}, "manifest": None}
        if self.state_path.exists():
            self.state = json.loads(self.state_path.read_text())
        self.digest_cache = {}      # source frame -> (stat key, sha256) so a poll does not rehash frames
        self.total = TOTAL_FRAMES
        self.stop = False
        self.child = None
        self.last_progress = time.time()
        self.tool_chem = None
        self.tool_rise = None
        self.failures = 0

    # -- helpers
    def save_state(self):
        write_atomic(self.state_path, json.dumps(self.state, indent=1) + "\n")

    def run_finished(self):
        return (self.run / SUMMARY_SEAL).is_file()

    # -- source frames
    def source_frame(self, k):
        """(vdb path, sim time, sha256) when frame k is complete, else None."""
        name = "frame_%05d" % k
        vdb = self.run / "checkpoints" / (name + ".vdb")
        record = self.run / "checkpoints" / (name + ".time")
        if vdb.is_file() and record.is_file():
            lines = record.read_text().split()
            if len(lines) == 2 and re.fullmatch(r"[0-9a-f]{64}", lines[1]):
                try:
                    stat = vdb.stat()
                except OSError:
                    return None
                key = (stat.st_size, stat.st_mtime_ns)
                cached = self.digest_cache.get(k)
                if not cached or cached[0] != key:
                    cached = (key, sha256_file(vdb))
                    self.digest_cache[k] = cached
                if cached[1] == lines[1]:
                    return vdb, float(lines[0]), lines[1]
            return None
        # finished run without checkpoints/: frames/ + frame_times.csv
        vdb = self.run / "frames" / (name + ".vdb")
        times = self.run / "frame_times.csv"
        if vdb.is_file() and times.is_file() and self.run_finished():
            for row in csv.reader(open(times)):
                if row and row[0] == str(k):
                    if sha256_file(vdb) == row[2]:
                        return vdb, float(row[1]), row[2]
        return None

    def ready_prefix(self):
        frames = []
        for k in range(self.total):
            frame = self.source_frame(k)
            if frame is None:
                break
            frames.append(frame)
        return frames

    # -- derive
    def derive(self, frames):
        done = 0
        for k, (vdb, _, digest) in enumerate(frames):
            target = self.seq / "frames" / ("frame_%05d.vdb" % k)
            record = self.state["derived"].get(str(k))
            if record and record["src_sha256"] == digest and target.is_file() and \
                    target.stat().st_size == record["size"]:
                continue
            tmp = target.with_name(target.name + ".part")
            result = subprocess.run([str(self.tool_chem), str(vdb), str(tmp), self.args.chem_fraction],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
            if result.returncode != 0:
                tmp.unlink(missing_ok=True)
                raise RuntimeError("chem_from_reaction failed on frame %d (rc=%d) %s" % (k, result.returncode, result.stderr))
            os.replace(tmp, target)
            self.state["derived"][str(k)] = {"src_sha256": digest, "sha256": sha256_file(target),
                                             "size": target.stat().st_size}
            done += 1
            self.save_state()
        if done:
            log("derived %d frame(s); %d available" % (done, len(frames)))

    # -- manifest
    def write_frame_times(self, frames, count):
        rows = [["frame", "simulation_time_s", "vdb_sha256"]]
        for k in range(count):
            rows.append([k, repr(frames[k][1]), self.state["derived"][str(k)]["sha256"]])
        tmp = self.seq / "frame_times.csv.tmp"
        with open(tmp, "w", newline="") as f:
            csv.writer(f, lineterminator="\n").writerows(rows)
        os.replace(tmp, self.seq / "frame_times.csv")

    def solver_manifest(self, frames):
        """Largest usable solver manifest bytes, or None. Usable: parses canonically and agrees with the verified
        digests of the source frames it lists."""
        best = None
        for name in (FINAL_MANIFEST, PARTIAL_MANIFEST):
            path = self.run / name
            if not path.is_file():
                continue
            try:
                data = path.read_bytes()
                digests = manifest_frame_digests(data)
            except Exception as error:
                log("solver manifest %s unusable: %s" % (name, error))
                continue
            usable = 0
            for k, digest in enumerate(digests):
                if k < len(frames) and frames[k][2] == digest:
                    usable += 1
                else:
                    break
            if usable >= 2 and (best is None or usable > best[0]):
                best = (usable, data, name)
        return best

    def ensure_manifest(self, frames):
        """Make seq/sequence_manifest.rise-fire.cbor cover frames 0..K-1; returns K (0 = not yet possible)."""
        available = len(frames)
        if available < 2 or not (self.run / "window_launch.v1").is_file():
            return 0
        mode = self.args.manifest
        method, count, solver = None, 0, None
        if mode in ("auto", "solver"):
            solver = self.solver_manifest(frames)
            if solver:
                method, count = "solver:" + solver[2], min(available, solver[0])
            elif mode == "solver":
                return 0
        if method is None:
            method, count = "rebuild", available
        current = self.state.get("manifest")
        manifest_path = self.seq / FINAL_MANIFEST
        if current and current["frames"] >= count and manifest_path.is_file():
            return current["frames"]
        # frames the manifest will list must all be derived
        for k in range(count):
            if str(k) not in self.state["derived"]:
                count = k
                break
        if count < 2:
            return 0
        shutil.copy2(self.run / "window_launch.v1", self.seq / "window_launch.v1")
        self.write_frame_times(frames, count)
        started = time.time()
        if method.startswith("solver:"):
            entries = [("frames/frame_%05d.vdb" % k, self.state["derived"][str(k)]["sha256"]) for k in range(count)]
            write_atomic(manifest_path, repoint_manifest(solver[1], entries))
        else:
            count = self.rebuild_manifest(frames, count, manifest_path)
        self.state["manifest"] = {"frames": count, "method": method, "sha256": sha256_file(manifest_path)}
        self.save_state()
        log("manifest covers %d frame(s) via %s (%.1f s)" % (count, method, time.time() - started))
        return count

    def rebuild_manifest(self, frames, count, manifest_path):
        """FireSequenceTest --fire-production-manifest-rebuild over seq/ (frames 0..count-1 only: the rebuild reads
        frame_times.csv, so the csv written by write_frame_times is the whole frame list)."""
        tool = snapshot_binary(ROOT / "bin/tests/FireSequenceTest", self.tools / "FireSequenceTest", False)
        env = {k: v for k, v in os.environ.items() if not k.startswith("RISE_FIRE_")}
        env.pop("RISE_MEDIA_PATH", None)
        env.update(REBUILD_ENV)
        env.update(self.args.rebuild_env)
        candidate = self.out / "tmp_manifest.cbor"
        candidate.unlink(missing_ok=True)
        with open(self.out / "logs" / "manifest_rebuild.log", "a") as logfile:
            result = subprocess.run([str(tool), "--fire-production-manifest-rebuild", "8", "0.0625", str(self.seq),
                                     str(candidate)], cwd=ROOT, env=env, stdout=logfile, stderr=subprocess.STDOUT)
        if result.returncode != 0 or not candidate.is_file():
            raise RuntimeError("manifest rebuild failed (rc=%d); see logs/manifest_rebuild.log" % result.returncode)
        os.replace(candidate, manifest_path)
        return count

    # -- render
    def write_scene(self):
        scene = self.out / "scene.RISEscene"
        text = SCENE_TEMPLATE.read_text().replace("@OUT@", str(self.out)).replace("@SEQ@", str(self.seq))
        text += "\nanimation_options\n{\ntime_start 0\ntime_end %s\nnum_frames %d\n}\n" % (
            repr((self.total - 1) * CADENCE_S), self.total)
        if not scene.exists() or scene.read_text() != text:
            scene.write_text(text)
        (self.out / "threads.options").write_text("force_number_of_threads %d\n" % self.args.threads)
        return scene

    def png_ok(self, k):
        path = self.out / ("fire_display_denoised%04d.png" % k)
        try:
            with open(path, "rb") as f:
                head = f.read(8)
                f.seek(-12, os.SEEK_END)
                tail = f.read(12)
            return head == b"\x89PNG\r\n\x1a\n" and tail[4:8] == b"IEND"
        except OSError:
            return False

    def is_rendered(self, k):
        record = self.state["rendered"].get(str(k))
        derived = self.state["derived"].get(str(k))
        return bool(record and derived and record["derived_sha256"] == derived["sha256"] and self.png_ok(k))

    def render_batch(self, batch, scene, covered):
        env = dict(os.environ, RISE_MEDIA_PATH="/", RISE_OPTIONS_FILE=str(self.out / "threads.options"),
                   RISE_FIRE_SEQUENCE_PREFLIGHT="used")
        commands = "".join("renderanimation %d\n" % k for k in batch) + "quit\n"
        number = len(list((self.out / "logs").glob("render_*.log"))) + 1
        logpath = self.out / "logs" / ("render_%04d_frames_%d-%d.log" % (number, batch[0], batch[-1]))
        started = time.time()
        marker = time.time_ns()
        nice = self.args.nice
        with open(logpath, "w") as logfile:
            self.child = subprocess.Popen([str(self.tool_rise), "--width", str(self.args.width), "--height",
                                           str(self.args.height), str(scene)], stdin=subprocess.PIPE,
                                          stdout=logfile, stderr=subprocess.STDOUT, cwd=ROOT, env=env, text=True,
                                          preexec_fn=(lambda: os.nice(nice)) if nice else None)
            try:
                self.child.communicate(commands)
            finally:
                code = self.child.returncode
                self.child = None
        elapsed = time.time() - started
        good = []
        for k in batch:
            path = self.out / ("fire_display_denoised%04d.png" % k)
            if self.png_ok(k) and path.stat().st_mtime_ns >= marker:
                self.state["rendered"][str(k)] = {"derived_sha256": self.state["derived"][str(k)]["sha256"],
                                                  "png_sha256": sha256_file(path), "manifest_frames": covered,
                                                  "finished_utc": datetime.datetime.now(datetime.timezone.utc).isoformat()}
                good.append(k)
        self.save_state()
        log("rendered frame(s) %s in %.1f s (%.1f s/frame, rc=%s)" % (good, elapsed, elapsed / max(1, len(batch)), code))
        if good:
            self.last_progress = time.time()
        if len(good) != len(batch):
            log("render did not produce frame(s) %s; see %s" % (sorted(set(batch) - set(good)), logpath))
        return len(good)

    # -- movie
    def assemble(self, count, partial):
        name = self.args.name
        if partial:
            name = re.sub(r"\.mov$", "", name) + "_partial_%df.mov" % count
        movie = self.out / name
        if movie.exists():
            if not partial:
                log("movie exists: " + str(movie))
                return movie
            movie.unlink()
        for script in ("r242_encode.swift", "r242_verify.swift"):
            used = ROOT / "tools/fire" / script
            if count != TOTAL_FRAMES:   # the scripts hard-code 235 frames; a partial movie patches the count
                used = self.out / ("partial_" + script)
                used.write_text((ROOT / "tools/fire" / script).read_text().replace("235", str(count)))
            args = [str(self.out), str(movie), str(self.args.width), str(self.args.height)] if "encode" in script \
                else [str(movie), str(self.args.width), str(self.args.height)]
            with open(self.out / ("encode.log" if "encode" in script else "verify.log"), "w") as logfile:
                subprocess.run(["swift", str(used), *args], cwd=ROOT, stdout=logfile, stderr=subprocess.STDOUT, check=True)
        record = dict(movie=str(movie), sha256=sha256_file(movie), frames=count, width=self.args.width,
                      height=self.args.height, fps=16, partial=partial)
        write_atomic(self.out / ("partial.json" if partial else "complete.json"), json.dumps(record, indent=2) + "\n")
        log("movie %s (%d frames)" % (movie, count))
        return movie

    def contiguous_rendered(self):
        k = 0
        while k < self.total and self.is_rendered(k):
            k += 1
        return k

    # -- main loop
    def request_stop(self, *_):
        self.stop = True
        if self.child is not None:
            self.child.terminate()

    def write_launch_record(self):
        record = dict(started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), run_dir=str(self.run),
                      renderer_sha256=sha256_file(self.tool_rise), scene_sha256=sha256_file(self.out / "scene.RISEscene"),
                      width=self.args.width, height=self.args.height, frames=self.total, fps=16,
                      threads=self.args.threads, nice=self.args.nice, manifest_mode=self.args.manifest,
                      chem_fraction=self.args.chem_fraction, batch=self.args.batch)
        n = len(list(self.out.glob("launch*.json")))
        write_atomic(self.out / ("launch.json" if n == 0 else "launch_%d.json" % n), json.dumps(record, indent=2) + "\n")

    def main(self):
        signal.signal(signal.SIGTERM, self.request_stop)
        signal.signal(signal.SIGINT, self.request_stop)
        self.tool_chem = build_chem_tool(self.tools)
        self.tool_rise = snapshot_binary(pathlib.Path(os.environ.get("RISE_RENDER_BINARY", ROOT / "bin/rise")),
                                         self.tools / "rise", self.args.refresh_tools)
        scene = self.write_scene()
        self.write_launch_record()
        wanted = None if self.args.frames is None else set(self.args.frames)
        log("following %s -> %s (finished=%s)" % (self.run, self.out, self.run_finished()))
        errors = 0
        while not self.stop:
            try:
                result = self.step(scene, wanted)
                errors = 0
            except Exception as error:      # a live follow rides out a transient failure (disk, renderer, tool)
                errors += 1
                import traceback
                log("step failed (%d/10): %s\n%s" % (errors, error, traceback.format_exc()))
                if errors >= 10:
                    raise
                time.sleep(max(self.args.poll, 30.0))
                continue
            if result is self.IDLE:
                time.sleep(self.args.poll)
            elif result is not None:
                return result
        log("stopped on signal")
        return 130

    def step(self, scene, wanted):
        """One pass of the follow loop. Returns an exit code when the follower is done, None to keep going.
        After a render batch it returns at once (more frames may be waiting); the caller sleeps only when idle."""
        finished = self.run_finished()
        frames = self.ready_prefix()
        if wanted is not None:
            frames = frames[:max(wanted) + 1]
        if frames:
            self.derive(frames)
        covered = self.ensure_manifest(frames) if len(frames) >= 2 else 0
        pending = [k for k in range(covered) if (wanted is None or k in wanted) and not self.is_rendered(k)]
        if pending:
            if self.render_batch(pending[:self.args.batch], scene, covered) == 0 and not self.stop:
                self.failures += 1
                if self.failures >= 3:
                    raise RuntimeError("three renderer batches in a row produced no frame; see logs/")
                return self.IDLE
            self.failures = 0
            return None
        request = self.out / "assemble.request"
        if request.exists():
            n = self.contiguous_rendered()
            if n >= 2:
                self.assemble(n, partial=n != self.total)
            request.unlink(missing_ok=True)
        if self.args.assemble_now:
            n = self.contiguous_rendered()
            if n >= 2:
                self.assemble(n, partial=n != self.total)
            return 0
        if finished and len(frames) == self.total and self.contiguous_rendered() == self.total:
            if not (self.out / "complete.json").is_file() and not self.args.no_movie and wanted is None:
                self.assemble(self.total, partial=False)
            log("run finished and all %d frames rendered" % self.total)
            return 0
        if self.args.once or (wanted is not None and all(self.is_rendered(k) for k in wanted)):
            return 0
        if not finished and frames and time.time() - self.last_progress > self.args.stall_minutes * 60:
            log("no new frame for %d min and the run has not finished; still waiting" % self.args.stall_minutes)
            self.last_progress = time.time()
        return self.IDLE


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("run_dir")
    p.add_argument("out_dir")
    p.add_argument("--width", type=int, default=256)
    p.add_argument("--height", type=int, default=384)
    p.add_argument("--threads", type=int, default=8, help="renderer worker threads (default 8; the machine has 16 cores)")
    p.add_argument("--nice", type=int, default=10, help="nice value for the renderer (default 10, 0 = none)")
    p.add_argument("--batch", type=int, default=8, help="frames per renderer process (default 8)")
    p.add_argument("--poll", type=float, default=20.0, help="seconds between polls of the run directory")
    p.add_argument("--manifest", choices=("auto", "solver", "rebuild"), default="auto",
                   help="auto: the solver's manifest when usable, else the rebuild tool")
    p.add_argument("--chem-fraction", default="3.0e-6")
    p.add_argument("--name", default=None, help="movie file name (default follow_<run>_<W>x<H>.mov)")
    p.add_argument("--frames", type=lambda s: [int(x) for x in s.split(",")], default=None,
                   help="render only these frame indices (verification), then exit; no movie")
    p.add_argument("--once", action="store_true", help="process what is ready, then exit")
    p.add_argument("--assemble-now", action="store_true", help="render what is ready, then encode a partial movie and exit")
    p.add_argument("--no-movie", action="store_true")
    p.add_argument("--refresh-tools", action="store_true", help="re-copy bin/rise into the output dir")
    p.add_argument("--stall-minutes", type=int, default=30)
    p.add_argument("--rebuild-env", action="append", default=[], metavar="KEY=VALUE",
                   help="extra environment for the manifest rebuild tool")
    args = p.parse_args()
    args.rebuild_env = dict(item.split("=", 1) for item in args.rebuild_env)
    if args.name is None:
        args.name = "follow_%s_%dx%d.mov" % (pathlib.Path(args.run_dir).resolve().name, args.width, args.height)
    return Follower(args).main()


if __name__ == "__main__":
    sys.exit(main())
