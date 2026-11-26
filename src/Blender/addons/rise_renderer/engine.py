import array
import bpy
import math
import threading
import time

from . import bridge, exporter, material_bake
from .properties import addon_preferences


# Phase 7b — the auto-dispatcher's most recent resolution, surfaced in the
# Properties > Render panel (ui.py).  Process-wide + transient (not per-scene,
# not saved); set after each render in RISEBlenderRenderEngine.render().
_LAST_AUTO_RESOLUTION = {"is_auto": False, "integrator": "", "reason": ""}


def get_last_auto_resolution():
    """The {is_auto, integrator, reason} of the most recent render's auto
    dispatcher, for the UI panel.  is_auto is False for a non-auto rasterizer."""
    return dict(_LAST_AUTO_RESOLUTION)


def _format_duration(seconds: float) -> str:
    """Format a duration in Blender/Cycles render-header timecode form:
    ``MM:SS.CC`` below one hour, ``HH:MM:SS.CC`` at or above one hour.
    Non-finite or negative inputs clamp to ``00:00.00``."""
    if not isinstance(seconds, (int, float)) or not math.isfinite(seconds) or seconds <= 0.0:
        return "00:00.00"
    clamped = min(float(seconds), 359999.99)
    total_cs = int(round(clamped * 100.0))
    cs = total_cs % 100
    total_s = total_cs // 100
    s = total_s % 60
    m = (total_s // 60) % 60
    h = total_s // 3600
    if h > 0:
        return f"{h:02d}:{m:02d}:{s:02d}.{cs:02d}"
    return f"{m:02d}:{s:02d}.{cs:02d}"


def _live_remaining_seconds(
    remaining_seconds: float,
    has_remaining: bool,
    received_at: float,
    now: float,
) -> float:
    """Count down a RenderETAEstimator remaining-time snapshot in real time
    between progress callbacks (mirrors RISE::RenderETAEstimator::RemainingSeconds)."""
    if not has_remaining or not math.isfinite(remaining_seconds):
        return -1.0
    dt = max(0.0, float(now) - float(received_at))
    return max(0.0, float(remaining_seconds) - dt)


def _normalize_stage_title(title: str) -> str:
    cleaned = (title or "").strip().rstrip(":").strip()
    return cleaned or "Rendering"


def _format_render_status(
    title: str,
    progress: float,
    remaining_seconds: float,
    has_remaining: bool,
) -> str:
    """Build the non-empty ``info`` string for ``RenderEngine.update_stats("", info)``.

    Keeping ``info`` non-empty activates Blender's live elapsed ``Time:`` readout
    in the Render View header (and prints clean ``Fra: N | <info>`` lines in
    background mode); passing ``""`` for ``stats`` matches Cycles and avoids
    replacing the stats column with a static engine label."""
    stage = _normalize_stage_title(title)
    has_pct = isinstance(progress, (int, float)) and math.isfinite(progress) and progress >= 0.0
    if has_pct:
        pct = max(0, min(100, int(progress * 100.0)))
        stage_text = f"{stage} {pct}%"
    else:
        stage_text = stage

    if (
        has_remaining
        and isinstance(remaining_seconds, (int, float))
        and math.isfinite(remaining_seconds)
        and (not has_pct or progress < 1.0)
    ):
        return f"Remaining: {_format_duration(max(0.0, remaining_seconds))} | {stage_text}"
    return stage_text


def _should_refresh_stats(
    *,
    is_background: bool,
    stage_changed: bool,
    pct_changed: bool,
    eta_became_ready: bool,
    elapsed_since_update: float,
    status_interval: float = 0.25,
) -> bool:
    """Decide whether to emit a new ``RenderEngine.update_stats`` call.

    In GUI mode, refreshes on every stage change, at least every
    ``status_interval`` seconds (keeping Blender's ``Time:`` and ``Remaining:``
    ticking smoothly), or promptly when integer percentage advances.
    In headless/background mode, refreshes only on meaningful transitions
    (stage change, percentage advance, or ETA warmup completion) so stdout
    stays clean."""
    if stage_changed:
        return True
    if is_background:
        return pct_changed or eta_became_ready
    if elapsed_since_update >= status_interval:
        return True
    if pct_changed and elapsed_since_update >= 0.05:
        return True
    return eta_became_ready


class RISEBlenderRenderEngine(bpy.types.RenderEngine):
    bl_idname = "RISE_RENDER"
    bl_label = "RISE"
    bl_use_preview = True

    @staticmethod
    def _set_pass_rect(render_pass, pixels):
        try:
            render_pass.rect = pixels
        except TypeError:
            render_pass.rect = [
                tuple(pixels[index:index + 4])
                for index in range(0, len(pixels), 4)
            ]

    @staticmethod
    def _extract_flipped_region(
        pixels,
        full_width,
        full_height,
        crop_left,
        crop_top,
        crop_width,
        crop_height,
    ):
        expected_full = full_width * full_height * 4
        if (
            full_width <= 0
            or full_height <= 0
            or crop_width <= 0
            or crop_height <= 0
            or len(pixels) != expected_full
        ):
            return pixels

        cropped = array.array("f", [0.0]) * (crop_width * crop_height * 4)
        dst_row_values = crop_width * 4
        src_row_stride = full_width * 4
        for row in range(crop_height):
            src_y = crop_top + row
            if src_y < 0 or src_y >= full_height:
                continue
            src_x0 = max(crop_left, 0)
            src_x1 = min(crop_left + crop_width, full_width)
            if src_x1 <= src_x0:
                continue
            copy_values = (src_x1 - src_x0) * 4
            src_base = src_y * src_row_stride + src_x0 * 4
            # Blender render passes expect rows in bottom-up order.
            dst_y = crop_height - 1 - row
            dst_base = dst_y * dst_row_values + (src_x0 - crop_left) * 4
            cropped[dst_base:dst_base + copy_values] = array.array(
                "f", pixels[src_base:src_base + copy_values]
            )
        return cropped

    @staticmethod
    def _flip_full_image_rows(pixels, width, height):
        return RISEBlenderRenderEngine._extract_flipped_region(
            pixels, width, height, 0, 0, width, height
        )

    @staticmethod
    def _apply_region_to_buffer(
        buffer,
        rgba16_bytes,
        region_width,
        region_height,
        full_width,
        full_height,
        region_top,
        region_left,
        crop_left=0,
        crop_top=0,
        crop_width=None,
        crop_height=None,
    ):
        if crop_width is None:
            crop_width = full_width
        if crop_height is None:
            crop_height = full_height
        if (
            region_width <= 0
            or region_height <= 0
            or full_width <= 0
            or full_height <= 0
            or crop_width <= 0
            or crop_height <= 0
        ):
            return

        if (
            region_left < 0
            or region_top < 0
            or (region_left + region_width) > full_width
            or (region_top + region_height) > full_height
        ):
            return

        values = array.array("H")
        values.frombytes(rgba16_bytes)
        expected_value_count = region_width * region_height * 4
        if len(values) != expected_value_count:
            return

        # Clip incoming full-frame tile against the active crop window.
        x0 = max(region_left, crop_left)
        x1 = min(region_left + region_width, crop_left + crop_width)
        y0 = max(region_top, crop_top)
        y1 = min(region_top + region_height, crop_top + crop_height)
        if x1 <= x0 or y1 <= y0:
            return

        scale = 1.0 / 65535.0
        copy_cols = x1 - x0
        for full_y in range(y0, y1):
            src_row = full_y - region_top
            src_col = x0 - region_left
            src_base = (src_row * region_width + src_col) * 4
            # Blender render passes expect rows in bottom-up order.
            dst_row = crop_height - 1 - (full_y - crop_top)
            dst_col = x0 - crop_left
            dst_base = (dst_row * crop_width + dst_col) * 4
            for offset in range(copy_cols * 4):
                buffer[dst_base + offset] = values[src_base + offset] * scale

    def update_render_passes(self, scene, render_layer):
        if render_layer.use_pass_combined:
            self.register_pass(scene, render_layer, "Combined", 4, "RGBA", "COLOR")

    def render(self, depsgraph):
        preferences = addon_preferences()
        bridge_path = preferences.bridge_library_path if preferences else None

        try:
            # Material-bake gate: detect complex Cycles material graphs
            # with viable proxies and missing/mismatched attempted fingerprints; then
            # abort with a clear, actionable message pointing at the
            # `rise.bake_materials` operator.
            #
            # Rationale for the two-click UX over auto-bake-on-render:
            # `bpy.ops.object.bake` was designed to run from a user
            # click in a 3D View, not from inside a render callback.
            # We tried wiring an auto-bake into the `render_init`
            # handler with full `bpy.context.temp_override`, and hit
            # four cascading Blender-API failure modes — engine-swap
            # vtable corruption, restricted-context AttributeError on
            # `selected_objects`, `hide_render`-blocked instanced
            # objects, and finally a libc malloc double-free on the
            # bake-target image teardown.  Each fix unlocked the next;
            # the auto-bake-on-render architecture is fundamentally
            # fighting the bake operator's preconditions. These are historical
            # failed attempts; the supported channel workflow is the manual
            # operator, independent of the automatic Tangent child bake.
            scene = depsgraph.scene
            # `needs_bake_attempt` is the single source of truth that
            # `RISE_RENDER_PT_materials` also uses — they stay aligned
            # in lockstep.  Returns True iff the user should run the
            # bake operator before rendering (missing marker OR detected limited
            # fingerprint change). Undetected edits do not gate. A tried
            # attempt that failed is not flagged while its fingerprint matches.
            # Existing diffuse may be consumed; otherwise ordinary translation/
            # slot/default fallback applies.
            # Missing PNGs alone do not gate a matching attempt. Tangent
            # uses a separate isolated export-time child, not this driver.
            missing = [
                mat.name
                for mat in material_bake.materials_needing_bake(scene, depsgraph=depsgraph)
            ]
            if missing:
                names = ", ".join(missing[:3])
                if len(missing) > 3:
                    names += f", … +{len(missing) - 3} more"
                msg = (
                    f"RISE: {len(missing)} procedural material(s) need baking "
                    f"({names}). Click Properties → Render → RISE Material "
                    f"Baking → Bake Procedural Materials, then re-render."
                )
                self.report({"ERROR"}, msg)
                self.error_set(msg)
                return

            self.update_stats("", "Exporting scene")
            scene_data, render_settings = exporter.export_scene(depsgraph)

            for warning in scene_data.warnings:
                self.report({"WARNING"}, warning)

            self.update_stats("", "Building scene")
        except Exception as exc:
            message = str(exc)
            self.report({"ERROR"}, message)
            self.error_set(message)
            return

        state_lock = threading.Lock()
        cancel_requested = threading.Event()
        progress_state = {
            "seq": 0,
            "progress": -1.0,
            "elapsed": 0.0,
            "remaining": -1.0,
            "has_remaining": False,
            "title": "Building scene",
            "received_at": time.monotonic(),
        }
        preview_state = {"payloads": []}
        final_state = {"done": False, "image": None, "error": None}
        if getattr(render_settings, "use_region", False):
            crop_left = int(render_settings.region_left)
            crop_top = int(render_settings.region_top)
            crop_width = int(render_settings.region_right) - crop_left + 1
            crop_height = int(render_settings.region_bottom) - crop_top + 1
        else:
            crop_left = 0
            crop_top = 0
            crop_width = int(render_settings.width)
            crop_height = int(render_settings.height)
        result = self.begin_result(0, 0, crop_width, crop_height)
        # Always match Blender's allocated pass extents (guards against any
        # 1px border rounding edge case between Python and Blender C), and
        # keep the rasterizer's region bounds synchronized before rendering.
        crop_width = int(result.resolution_x)
        crop_height = int(result.resolution_y)
        if getattr(render_settings, "use_region", False) and crop_width > 0 and crop_height > 0:
            render_settings.region_right = min(int(render_settings.width) - 1, crop_left + crop_width - 1)
            render_settings.region_bottom = min(int(render_settings.height) - 1, crop_top + crop_height - 1)
        preview_buffer = array.array("f", [0.0]) * (crop_width * crop_height * 4)
        layer = result.layers[0].passes["Combined"]

        def _queue_progress_info(progress_value, elapsed_seconds, remaining_seconds, has_remaining, title):
            now_mono = time.monotonic()
            with state_lock:
                progress_state["seq"] += 1
                progress_state["progress"] = float(progress_value)
                progress_state["elapsed"] = float(elapsed_seconds)
                progress_state["remaining"] = float(remaining_seconds)
                progress_state["has_remaining"] = bool(has_remaining)
                if title:
                    progress_state["title"] = title
                progress_state["received_at"] = now_mono
            return not cancel_requested.is_set()

        def _queue_image(rgba16_bytes, region_width, region_height, full_width, full_height, region_top, region_left):
            with state_lock:
                preview_state["payloads"].append(
                    (
                        rgba16_bytes,
                        region_width,
                        region_height,
                        full_width,
                        full_height,
                        region_top,
                        region_left,
                    )
                )
            return not cancel_requested.is_set()

        def _render_worker():
            try:
                image = bridge.render_scene(
                    scene_data,
                    render_settings,
                    bridge_path=bridge_path,
                    progress_info=_queue_progress_info,
                    image_update=_queue_image,
                )
                with state_lock:
                    final_state["image"] = image
                    final_state["done"] = True
            except Exception as exc:
                with state_lock:
                    final_state["error"] = str(exc)
                    final_state["done"] = True

        worker = threading.Thread(target=_render_worker, name="RISEBridgeRender", daemon=True)
        worker.start()

        last_progress_seq = -1
        last_stats_update = time.monotonic()
        last_stats_title = "Building scene"
        last_stats_pct = -1
        last_stats_has_remaining = False
        last_preview_update = 0.0
        status_interval = 0.25
        preview_interval = 0.1
        preview_dirty = False
        is_background = bool(getattr(bpy.app, "background", False))

        while True:
            with state_lock:
                progress_seq = progress_state["seq"]
                progress_value = progress_state["progress"]
                remaining_seconds = progress_state["remaining"]
                has_remaining = progress_state["has_remaining"]
                progress_title = progress_state["title"]
                received_at = progress_state["received_at"]
                preview_payloads = preview_state["payloads"]
                preview_state["payloads"] = []
                done = final_state["done"]
                image = final_state["image"]
                error = final_state["error"]

            now = time.monotonic()

            if progress_seq != last_progress_seq:
                last_progress_seq = progress_seq
                if progress_value >= 0.0 and math.isfinite(progress_value):
                    self.update_progress(max(0.0, min(progress_value, 1.0)))

            pct = (
                max(0, min(100, int(progress_value * 100.0)))
                if (progress_value >= 0.0 and math.isfinite(progress_value))
                else -1
            )
            stage_changed = progress_title != last_stats_title
            pct_changed = pct != last_stats_pct
            eta_became_ready = bool(has_remaining and not last_stats_has_remaining)
            if _should_refresh_stats(
                is_background=is_background,
                stage_changed=stage_changed,
                pct_changed=pct_changed,
                eta_became_ready=eta_became_ready,
                elapsed_since_update=now - last_stats_update,
                status_interval=status_interval,
            ):
                live_remaining = _live_remaining_seconds(
                    remaining_seconds,
                    has_remaining,
                    received_at,
                    now,
                )
                status_text = _format_render_status(
                    progress_title,
                    progress_value,
                    live_remaining,
                    has_remaining,
                )
                self.update_stats("", status_text)
                last_stats_update = now
                last_stats_title = progress_title
                last_stats_pct = pct
                last_stats_has_remaining = bool(has_remaining)

            if preview_payloads:
                for payload in preview_payloads:
                    self._apply_region_to_buffer(
                        preview_buffer,
                        *payload,
                        crop_left=crop_left,
                        crop_top=crop_top,
                        crop_width=crop_width,
                        crop_height=crop_height,
                    )
                preview_dirty = True

            if preview_dirty and (now - last_preview_update) >= preview_interval:
                last_preview_update = now
                self._set_pass_rect(layer, preview_buffer)
                self.update_result(result)
                preview_dirty = False

            if done:
                break

            if self.test_break():
                cancel_requested.set()

            time.sleep(0.01)

        worker.join(timeout=0.1)

        if error:
            self.end_result(result, cancel=True)
            if cancel_requested.is_set() and "canceled" in error.lower():
                self.update_stats("", "Canceled")
                return

            self.report({"ERROR"}, error)
            self.error_set(error)
            return

        # ABI v9 — non-fatal diagnostics from the native side (a groom
        # whose .hair file went missing, a hair material RISE refused).
        # Same channel the exporter's own warnings use, so the artist
        # sees one list regardless of which half produced the message.
        for warning in getattr(image, "warnings", ()) or ():
            self.report({"WARNING"}, warning)

        self._set_pass_rect(
            layer,
            self._extract_flipped_region(
                image.rgba,
                image.width,
                image.height,
                crop_left,
                crop_top,
                crop_width,
                crop_height,
            ),
        )
        self.update_result(result)
        self.end_result(result)

        self.update_progress(1.0)

        # Phase 7b — surface the auto-dispatcher's resolution: the render stats
        # line + an INFO report (Blender info bar / console), and stash it for
        # the Properties panel (ui.py reads get_last_auto_resolution()).
        global _LAST_AUTO_RESOLUTION
        if getattr(image, "is_auto", False) and image.resolved_integrator:
            summary = f"Auto \u2192 {image.resolved_integrator.upper()}"
            _LAST_AUTO_RESOLUTION = {
                "is_auto": True,
                "integrator": image.resolved_integrator,
                "reason": image.resolve_reason,
            }
            self.update_stats("", f"RISE  \u00b7  {image.width} x {image.height}  \u00b7  {summary}")
            self.report({"INFO"}, f"RISE: {summary} ({image.resolve_reason})")
        else:
            _LAST_AUTO_RESOLUTION = {"is_auto": False, "integrator": "", "reason": ""}
            self.update_stats("", f"RISE  \u00b7  {image.width} x {image.height}")


def register():
    bpy.utils.register_class(RISEBlenderRenderEngine)


def unregister():
    bridge.shutdown()
    bpy.utils.unregister_class(RISEBlenderRenderEngine)
