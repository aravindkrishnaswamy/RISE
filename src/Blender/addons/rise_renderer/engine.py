import array
import bpy
import math
import threading
import time

from . import bridge, exporter
from .properties import addon_preferences


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
    def _flip_full_image_rows(pixels, width, height):
        expected_value_count = width * height * 4
        if width <= 0 or height <= 0 or len(pixels) != expected_value_count:
            return pixels

        flipped = array.array("f", [0.0]) * expected_value_count
        row_width = width * 4
        for src_row in range(height):
            src_base = src_row * row_width
            dst_base = (height - 1 - src_row) * row_width
            row_values = array.array("f", pixels[src_base:src_base + row_width])
            flipped[dst_base:dst_base + row_width] = row_values

        return flipped

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
    ):
        if (
            region_width <= 0
            or region_height <= 0
            or full_width <= 0
            or full_height <= 0
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

        scale = 1.0 / 65535.0
        for row in range(region_height):
            src_base = row * region_width * 4
            # Blender render passes expect rows in bottom-up order.
            dst_row = full_height - 1 - (region_top + row)
            dst_base = (dst_row * full_width + region_left) * 4
            for offset in range(region_width * 4):
                buffer[dst_base + offset] = values[src_base + offset] * scale

    def _progress(self, progress, title):
        if title:
            info = "" if progress < 0.0 else f"{max(int(progress * 100.0), 0)}%"
            self.update_stats(title, info)

        if progress >= 0.0 and math.isfinite(progress):
            self.update_progress(max(0.0, min(progress, 1.0)))

        return not self.test_break()

    def update_render_passes(self, scene, render_layer):
        if render_layer.use_pass_combined:
            self.register_pass(scene, render_layer, "Combined", 4, "RGBA", "COLOR")

    def render(self, depsgraph):
        preferences = addon_preferences()
        bridge_path = preferences.bridge_library_path if preferences else None

        try:
            self.update_stats("RISE", "Exporting scene")
            scene_data, render_settings = exporter.export_scene(depsgraph)

            for warning in scene_data.warnings:
                self.report({"WARNING"}, warning)

            self.update_stats("RISE", "Rendering")
        except Exception as exc:
            message = str(exc)
            self.report({"ERROR"}, message)
            self.error_set(message)
            return

        state_lock = threading.Lock()
        cancel_requested = threading.Event()
        progress_state = {"seq": 0, "progress": -1.0, "title": ""}
        preview_state = {"payloads": []}
        final_state = {"done": False, "image": None, "error": None}
        preview_buffer = array.array("f", [0.0]) * (render_settings.width * render_settings.height * 4)
        result = self.begin_result(0, 0, render_settings.width, render_settings.height)
        layer = result.layers[0].passes["Combined"]

        def _queue_progress(progress_value, title):
            with state_lock:
                progress_state["seq"] += 1
                progress_state["progress"] = float(progress_value)
                progress_state["title"] = title
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
                    progress=_queue_progress,
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
        last_preview_update = 0.0
        preview_interval = 0.1
        preview_dirty = False

        while True:
            with state_lock:
                progress_seq = progress_state["seq"]
                progress_value = progress_state["progress"]
                progress_title = progress_state["title"]
                preview_payloads = preview_state["payloads"]
                preview_state["payloads"] = []
                done = final_state["done"]
                image = final_state["image"]
                error = final_state["error"]

            if progress_seq != last_progress_seq:
                last_progress_seq = progress_seq
                self._progress(progress_value, progress_title)

            if preview_payloads:
                for payload in preview_payloads:
                    self._apply_region_to_buffer(preview_buffer, *payload)
                preview_dirty = True

            now = time.monotonic()
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
                self.update_stats("RISE", "Canceled")
                return

            self.report({"ERROR"}, error)
            self.error_set(error)
            return

        self._set_pass_rect(
            layer,
            self._flip_full_image_rows(image.rgba, image.width, image.height),
        )
        self.update_result(result)
        self.end_result(result)

        self.update_progress(1.0)
        self.update_stats("RISE", f"{image.width} x {image.height}")


def register():
    bpy.utils.register_class(RISEBlenderRenderEngine)


def unregister():
    bridge.shutdown()
    bpy.utils.unregister_class(RISEBlenderRenderEngine)
