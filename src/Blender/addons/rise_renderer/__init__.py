bl_info = {
    "name": "RISE Render Engine",
    "author": "RISE contributors",
    "version": (0, 1, 0),
    "blender": (4, 0, 0),
    "location": "Render Properties > Render Engine",
    "description": "Render Blender scenes with the Realistic Image Synthesis Engine",
    "support": "COMMUNITY",
    "category": "Render",
}

from . import engine, properties, ui


def register():
    properties.register()
    engine.register()
    ui.register()


def unregister():
    ui.unregister()
    engine.unregister()
    properties.unregister()

