import bpy
import math

from . import bridge, exporter
from .properties import addon_preferences


class RISEBlenderRenderEngine(bpy.types.RenderEngine):
    bl_idname = "RISE_RENDER"
    bl_label = "RISE"
    bl_use_preview = True

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

            # RISE may invoke progress updates from worker threads. Forwarding those
            # callbacks into Blender UI helpers is not thread-safe on macOS, so keep
            # native rendering synchronous here until we build a thread-safe bridge.
            self.update_stats("RISE", "Rendering")
            image = bridge.render_scene(
                scene_data,
                render_settings,
                bridge_path=bridge_path,
            )
        except Exception as exc:
            message = str(exc)
            self.report({"ERROR"}, message)
            self.error_set(message)
            return

        result = self.begin_result(0, 0, image.width, image.height)
        layer = result.layers[0].passes["Combined"]
        layer.rect = [tuple(image.rgba[index:index + 4]) for index in range(0, len(image.rgba), 4)]
        self.end_result(result)

        self.update_progress(1.0)
        self.update_stats("RISE", f"{image.width} x {image.height}")


def register():
    bpy.utils.register_class(RISEBlenderRenderEngine)


def unregister():
    bridge.shutdown()
    bpy.utils.unregister_class(RISEBlenderRenderEngine)
