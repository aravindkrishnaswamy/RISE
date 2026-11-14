"""Small Blender-version shims shared by the exporter and bake helpers."""

from __future__ import annotations


def uses_nodes(idblock) -> bool:
    """True when a Material / World shades through its node tree.

    Blender 5.0 made node trees mandatory: ``use_nodes`` is pinned True,
    assignment is ignored, and reading it raises a DeprecationWarning
    ahead of its removal in 6.0.  On 5.x the node tree's presence is the
    whole answer; 4.x still honours ``use_nodes = False``.
    """

    if idblock is None or getattr(idblock, "node_tree", None) is None:
        return False
    # Imported lazily: `tangent_bake` (bpy-free at import time) uses this.
    try:
        import bpy
        version = tuple(bpy.app.version)
    except ImportError:
        version = (0, 0, 0)
    if version >= (5, 0, 0):
        return True
    return bool(getattr(idblock, "use_nodes", True))
