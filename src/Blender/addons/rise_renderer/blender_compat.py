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


def rna_key(struct) -> int:
    """Identity key for a Blender RNA struct, for visited sets and caches.

    NEVER use ``id(node)`` or ``node_a is node_b`` for this.  Blender builds
    a fresh Python wrapper each time a non-ID struct (a node, a socket, a
    link) is read, so the wrapper is freed as soon as the loop moves on and
    CPython hands its ``id()`` to a later wrapper: two different nodes then
    compare as "already visited" and a graph walk skips branches.  Which
    branches depends on allocator reuse, so the result is fixed for a given
    graph shape but changes when an unrelated node is added.  ``==`` and
    ``as_pointer()`` compare the underlying Blender data and are stable for
    as long as it exists; ID datablocks (materials, images, objects) keep
    one wrapper, and this key is used for them too so that no ``id()``
    remains to be audited.  Plain Python stand-ins (the unit-test fakes)
    have no ``as_pointer`` and a stable ``id``.
    """

    as_pointer = getattr(struct, "as_pointer", None)
    return as_pointer() if as_pointer is not None else id(struct)
