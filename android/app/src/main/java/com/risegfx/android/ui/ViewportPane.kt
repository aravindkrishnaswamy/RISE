package com.risegfx.android.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.Image
import androidx.compose.ui.platform.LocalDensity
import com.risegfx.android.nativebridge.RiseNative

/**
 * Inset around the rendered image inside the viewport canvas Box.
 * The Image uses .padding(this), and [mapToImagePixel] subtracts the
 * same amount when converting pointer coords back to image-pixel
 * space.  Keep these two in lockstep — drift causes a visible
 * offset between where the user clicks and where the camera ray
 * gets generated, growing toward the corners of the viewport.
 */
private val ViewportImagePadding = 8.dp

/**
 * Tools mirror SceneEditController::Tool / RISEViewportTool, identical
 * to the macOS/Windows implementations.  Numeric values pass straight
 * through to nativeViewportSetTool().
 *
 * Object Translate / Rotate / Scale and the standalone Scrub tool are
 * intentionally omitted from [visibleInToolbar] — object editing is
 * too much complexity for the current state of the app, and timeline
 * scrubbing is driven directly by the bottom timeline bar.
 */
enum class ViewportTool(val rawValue: Int, val label: String, val tooltip: String) {
    Select(0,
        "Select",
        "Select — click an object in the viewport to make it the target of the next edit"),
    OrbitCamera(4,
        "Orbit",
        "Orbit Camera — drag to rotate the camera around the scene"),
    PanCamera(5,
        "Pan",
        "Pan Camera — drag to translate the camera in screen plane"),
    ZoomCamera(6,
        "Zoom",
        "Zoom Camera — drag to dolly the camera closer or farther"),
    RollCamera(8,
        "Roll",
        "Roll Camera — drag horizontally to roll the camera around the (camera→look-at) axis");

    companion object {
        /** Tools surfaced in the toolbar UI. */
        val visibleInToolbar: List<ViewportTool> = values().toList()
    }
}

/** Quick-pick preset attached to a property row.  Mirrors
 *  ParameterPreset on the C++ side. */
data class ViewportPropertyPreset(
    val label: String,    // shown to the user in the dropdown
    val value: String,    // parser-acceptable literal written through SetProperty
)

/**
 * One row of the descriptor-driven properties panel, mirroring
 * CameraProperty / ViewportProperty on the other platforms.
 */
data class ViewportPropertyRow(
    val name: String,
    val value: String,
    val description: String,
    val kind: Int,
    val editable: Boolean,
    val presets: List<ViewportPropertyPreset> = emptyList(),
)

@Composable
fun ViewportPane(
    modifier: Modifier = Modifier,
    frame: ImageBitmap?,
    hasAnimation: Boolean,
    interactionEnabled: Boolean,
    state: RenderState,
    progress: Float,
    elapsedMs: Long,
    remainingMs: Long?,
    onRender: () -> Unit,
    onCancel: () -> Unit,
    /// Bumped by [RenderViewModel] each time the underlying viewport
    /// controller is restarted (scene load, after a production
    /// render).  We re-apply the persisted [selectedTool] on every
    /// epoch change so the freshly-constructed controller (which
    /// defaults to Select internally) agrees with what the toolbar
    /// is highlighting.
    viewportEpoch: Int,
    /// Live scrub position — owned by [RenderViewModel] so the VM
    /// can pass it to nativeSetSceneTime before kicking a production
    /// render.  ViewportPane reads it for slider display and writes
    /// to it via [onSceneTimeChange] alongside each
    /// nativeViewportScrub tick.
    sceneTime: Float,
    onSceneTimeChange: (Float) -> Unit,
) {
    var selectedTool by rememberSaveable { mutableStateOf(ViewportTool.Select) }
    var properties by remember { mutableStateOf(emptyList<ViewportPropertyRow>()) }
    // SceneEditController::PanelMode — 0 None, 1 Camera, 2 Rasterizer,
    // 3 Object, 4 Light.  Mirrors SceneEditCategory_*.
    var panelMode by remember { mutableStateOf(0) }
    var panelHeader by remember { mutableStateOf("") }
    var refreshTrigger by remember { mutableStateOf(0) }
    var selectionCategory by remember { mutableStateOf(0) }
    var selectionName by remember { mutableStateOf("") }
    var entitiesByCategory by remember {
        mutableStateOf<Map<Int, List<String>>>(emptyMap())
    }
    var activeNameByCategory by remember {
        mutableStateOf<Map<Int, String>>(emptyMap())
    }
    var lastEpoch by remember { mutableStateOf(0) }

    // Re-sync the toolbar's selection to the underlying controller
    // each time the viewport restarts.  Without this, the toolbar's
    // saveable @State would still highlight (e.g.) Orbit while the
    // freshly-constructed controller is at Select internally.
    LaunchedEffect(viewportEpoch) {
        RiseNative.nativeViewportSetTool(selectedTool.rawValue)
    }

    LaunchedEffect(refreshTrigger) {
        if (interactionEnabled) {
            RiseNative.nativeViewportRefreshProperties()
            panelMode = RiseNative.nativeViewportPanelMode()
            panelHeader = RiseNative.nativeViewportPanelHeader()
            selectionCategory = RiseNative.nativeViewportSelectionCategory()
            selectionName = RiseNative.nativeViewportSelectionName()
            val n = RiseNative.nativeViewportPropertyCount()
            properties = (0 until n).map { i ->
                val pn = RiseNative.nativeViewportPropertyPresetCount(i)
                val presetList = if (pn > 0) {
                    (0 until pn).map { j ->
                        ViewportPropertyPreset(
                            label = RiseNative.nativeViewportPropertyPresetLabel(i, j),
                            value = RiseNative.nativeViewportPropertyPresetValue(i, j),
                        )
                    }
                } else {
                    emptyList()
                }
                ViewportPropertyRow(
                    name = RiseNative.nativeViewportPropertyName(i),
                    value = RiseNative.nativeViewportPropertyValue(i),
                    description = RiseNative.nativeViewportPropertyDescription(i),
                    kind = RiseNative.nativeViewportPropertyKind(i),
                    editable = RiseNative.nativeViewportPropertyEditable(i),
                    presets = presetList,
                )
            }

            // Re-pull per-section entity lists when the scene epoch
            // advances (scene reload, structural mutation).  The
            // four-section accordion lives over (Cameras, Rasterizer,
            // Objects, Lights), with category ints 1/2/3/4.
            val epoch = RiseNative.nativeViewportSceneEpoch()
            if (epoch != lastEpoch) {
                lastEpoch = epoch
                val fresh = mutableMapOf<Int, List<String>>()
                for (cat in intArrayOf(1, 2, 3, 4)) {
                    val nEntries = RiseNative.nativeViewportCategoryEntityCount(cat)
                    fresh[cat] = (0 until nEntries).map { idx ->
                        RiseNative.nativeViewportCategoryEntityName(cat, idx)
                    }
                }
                entitiesByCategory = fresh
            }

            // Active-name lookup is cheap (one JNI hop per category)
            // and can change between epochs (a SetActiveCamera can
            // happen without adding/removing cameras).  Re-pull every
            // refresh.
            val freshActive = mutableMapOf<Int, String>()
            for (cat in intArrayOf(1, 2, 3, 4)) {
                freshActive[cat] = RiseNative.nativeViewportCategoryActiveName(cat)
            }
            activeNameByCategory = freshActive
        }
    }

    // Bump the refresh trigger every time a new frame arrives — the
    // camera/object state may have just changed via a drag.  Tool
    // changes also bump it (panel mode is tool-derived).
    LaunchedEffect(frame) {
        refreshTrigger++
    }
    LaunchedEffect(selectedTool) {
        refreshTrigger++
    }

    Card(modifier, shape = RoundedCornerShape(16.dp)) {
        Column(Modifier.fillMaxSize().padding(8.dp)) {
            // Top: render-state strip with Render/Cancel button
            ViewportStateStrip(
                state = state,
                progress = progress,
                elapsedMs = elapsedMs,
                remainingMs = remainingMs,
                interactionEnabled = interactionEnabled,
                onRender = onRender,
                onCancel = onCancel,
            )
            Spacer(Modifier.height(8.dp))
            Row(Modifier.weight(1f).fillMaxWidth()) {
                // Left: properties panel.  Renders empty / camera /
                // object content based on `panelMode` (driven by the
                // active tool + selection on the C++ side).  Pinned
                // to the left so the panel sits next to the user's
                // input hand on a tablet held in landscape, leaving
                // the canvas filling the dominant screen real estate
                // on the right.
                Surface(
                    modifier = Modifier.width(280.dp).fillMaxHeight(),
                    tonalElevation = 1.dp,
                ) {
                    ViewportAccordionPanel(
                        properties = properties,
                        header = panelHeader,
                        mode = panelMode,
                        selectionCategory = selectionCategory,
                        selectionName = selectionName,
                        entitiesByCategory = entitiesByCategory,
                        activeNameByCategory = activeNameByCategory,
                        enabled = interactionEnabled,
                        onSelectionChanged = { cat, name ->
                            // Empty name = open the section without picking.
                            // Camera / Rasterizer selections also trigger
                            // scene mutations on the C++ side.
                            if (RiseNative.nativeViewportSetSelection(cat, name)) {
                                refreshTrigger++
                            }
                        },
                        onPropertyEdited = { name, value ->
                            if (RiseNative.nativeViewportSetProperty(name, value)) {
                                refreshTrigger++
                            }
                        },
                    )
                }
                Spacer(Modifier.width(8.dp))
                // Right: toolbar + canvas + timeline.
                Column(Modifier.weight(1f).fillMaxHeight()) {
                    ViewportToolbar(
                        selectedTool = selectedTool,
                        onToolSelected = {
                            selectedTool = it
                            RiseNative.nativeViewportSetTool(it.rawValue)
                        },
                        onUndo = { RiseNative.nativeViewportUndo() },
                        onRedo = { RiseNative.nativeViewportRedo() },
                        enabled = interactionEnabled,
                    )
                    Spacer(Modifier.height(8.dp))
                    ViewportCanvas(
                        modifier = Modifier.weight(1f).fillMaxWidth(),
                        frame = frame,
                        enabled = interactionEnabled,
                    )
                    if (hasAnimation) {
                        Spacer(Modifier.height(8.dp))
                        // Pull the timeline range from the scene's
                        // animation_options chunk.  Fallback to 5s
                        // when the scene declares no animation
                        // (timeEnd == 0) so a degenerate scene still
                        // renders a visible (if useless) slider.
                        val timelineEnd = remember {
                            val end = RiseNative.nativeViewportAnimationTimeEnd().toFloat()
                            if (end > 0f) end else 5f
                        }
                        ViewportTimelineSlider(
                            time = sceneTime,
                            timelineMax = timelineEnd,
                            onScrubBegin = { RiseNative.nativeViewportScrubBegin() },
                            onScrub = {
                                onSceneTimeChange(it)
                                RiseNative.nativeViewportScrub(it.toDouble())
                            },
                            onScrubEnd = { RiseNative.nativeViewportScrubEnd() },
                            enabled = interactionEnabled,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun ViewportStateStrip(
    state: RenderState,
    progress: Float,
    elapsedMs: Long,
    remainingMs: Long?,
    interactionEnabled: Boolean,
    onRender: () -> Unit,
    onCancel: () -> Unit,
) {
    Column(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            val label = when (state) {
                is RenderState.Idle        -> "Idle"
                is RenderState.Preparing   -> "Preparing: ${state.reason}"
                is RenderState.Loading     -> "Loading${'…'}"
                is RenderState.Rendering   -> "Rendering ${state.width}${'×'}${state.height} ${'·'} ${(progress * 100).toInt()}%"
                is RenderState.Cancelling  -> "Cancelling${'…'}"
                is RenderState.Done        -> "Done ${'·'} Elapsed ${formatDurationMs(elapsedMs)}"
                is RenderState.Cancelled   -> "Cancelled"
                is RenderState.Error       -> "Error: ${state.message}"
            }
            Text(label, style = MaterialTheme.typography.titleSmall)
            Spacer(Modifier.weight(1f))
            val isActive = state is RenderState.Rendering || state is RenderState.Cancelling
            if (isActive) {
                val remainingText = remainingMs?.let { "~${formatDurationMs(it)}" } ?: "${'…'}"
                Text("Remaining: $remainingText",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f))
                Spacer(Modifier.width(8.dp))
                OutlinedButton(onClick = onCancel) { Text("Cancel") }
            } else {
                OutlinedButton(onClick = onRender, enabled = interactionEnabled) { Text("Render") }
            }
        }
        if (state is RenderState.Rendering || state is RenderState.Cancelling) {
            Spacer(Modifier.height(4.dp))
            LinearProgressIndicator(
                progress = { progress.coerceIn(0f, 1f) },
                modifier = Modifier.fillMaxWidth().height(4.dp),
            )
        }
    }
}

@Composable
private fun ViewportToolbar(
    selectedTool: ViewportTool,
    onToolSelected: (ViewportTool) -> Unit,
    onUndo: () -> Unit,
    onRedo: () -> Unit,
    enabled: Boolean,
) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        ViewportTool.visibleInToolbar.forEach { tool ->
            val selected = (tool == selectedTool)
            FilterChip(
                selected = selected,
                enabled = enabled,
                onClick = { onToolSelected(tool) },
                label = { Text(tool.label, style = MaterialTheme.typography.labelSmall) },
                modifier = Modifier.padding(end = 4.dp),
            )
        }
        Spacer(Modifier.weight(1f))
        // ArrowBack/ArrowForward are guaranteed to exist in Icons.Default
        // across all material-icons versions; Undo/Redo aren't.  Same
        // semantics: backward = undo, forward = redo.
        IconButton(onClick = onUndo, enabled = enabled) {
            Icon(Icons.Default.ArrowBack, contentDescription = "Undo")
        }
        IconButton(onClick = onRedo, enabled = enabled) {
            Icon(Icons.Default.ArrowForward, contentDescription = "Redo")
        }
    }
}

/**
 * Map a Box-relative pointer offset to image-pixel space — the coord
 * system the rasterizer / camera use internally.  Aspect-fit centring
 * matches Image's `contentScale = ContentScale.Fit`: the image is
 * letterboxed inside the Box; we offset and rescale so coordinates
 * land in `0..surfaceW × 0..surfaceH`.
 *
 * `surfaceDims` holds the camera's STABLE full-resolution dimensions
 * (packed `Long` from [RiseNative.nativeViewportCameraDimensions]).
 * We do NOT use `frame.width / frame.height` for the conversion: the
 * rendered framebuffer's size shrinks during a fast drag (preview-
 * scale subsampling), and using its dims as the target makes mLastPx
 * (captured at one scale level) live in a different coord space from
 * the next event's px — the controller's (px - mLastPx) delta would
 * mix scale levels and produce 4×–32× pan/orbit jumps when the
 * preview-scale state machine steps.  `frame` is still required to
 * know the aspect-fit drawRect (Image's contentScale uses frame's
 * dims for the layout calculation, so we must match), but the
 * pixel-space target is the stable surface size.
 */
private fun mapToImagePixel(
    p: Offset, boxSize: IntSize, frame: ImageBitmap?, surfaceDims: Long,
    paddingPx: Int,
): Offset? {
    if (frame == null) return null
    if (boxSize.width <= 0 || boxSize.height <= 0) return null
    val frameW = frame.width.toFloat()
    val frameH = frame.height.toFloat()
    if (frameW <= 0f || frameH <= 0f) return null
    // The Image is rendered inside the Box with an inset of paddingPx
    // on every side (see ViewportImagePadding).  Pointer events are
    // reported in Box-space (no padding subtracted), so we shrink the
    // effective drawing area before doing the aspect-fit math —
    // otherwise a click in the corner of the Box maps to a position
    // outside the actual image, sending bogus camera rays to the
    // controller.
    val effW = (boxSize.width  - 2 * paddingPx).toFloat()
    val effH = (boxSize.height - 2 * paddingPx).toFloat()
    if (effW <= 0f || effH <= 0f) return null
    // Aspect-fit scale matches Image's ContentScale.Fit, which uses
    // the bitmap's intrinsic size for layout — so drawX/drawY are
    // computed against `frame.size`, not the surface size.
    val drawScale = minOf(effW / frameW, effH / frameH)
    val drawW = frameW * drawScale
    val drawH = frameH * drawScale
    val drawX = paddingPx + (effW - drawW) / 2f
    val drawY = paddingPx + (effH - drawH) / 2f
    if (drawW <= 0f || drawH <= 0f) return null

    // Pull the surface dims from the packed long (hi 32 = w, lo 32 = h).
    // Fall back to frame dims if the controller hasn't populated the
    // cache yet (returns 0).  The fallback path is exactly the
    // pre-fix behaviour, so we degrade gracefully on early frames.
    val surfW = ((surfaceDims ushr 32) and 0xffffffffL).toFloat()
    val surfH = (surfaceDims and 0xffffffffL).toFloat()
    val targetW = if (surfW > 0f) surfW else frameW
    val targetH = if (surfH > 0f) surfH else frameH

    val nx = (p.x - drawX) / drawW
    val ny = (p.y - drawY) / drawH
    return Offset(nx * targetW, ny * targetH)
}

@Composable
private fun ViewportCanvas(
    modifier: Modifier,
    frame: ImageBitmap?,
    enabled: Boolean,
) {
    var boxSize by remember { mutableStateOf(IntSize.Zero) }

    // dp → px conversion needs LocalDensity, which is only available
    // inside a @Composable scope — capture once here and pass as an
    // Int down into the gesture handlers.
    val paddingPx = with(LocalDensity.current) { ViewportImagePadding.roundToPx() }

    // pointerInput's coroutine is cancelled and restarted whenever any
    // key changes — and a restart cancels any in-flight detectDragGestures
    // mid-drag.  We keep `enabled` as a key because a render-state
    // transition really should drop the gesture, but `frame` and
    // `boxSize` change on every preview tile / layout pass and would
    // otherwise interrupt the user's drag at preview cadence.  Capture
    // them via rememberUpdatedState so the gesture handlers see the
    // latest values without rebinding.
    val frameState = rememberUpdatedState(frame)
    val boxSizeState = rememberUpdatedState(boxSize)
    val paddingPxState = rememberUpdatedState(paddingPx)
    Box(
        modifier
            .background(Color(0xFF101114), RoundedCornerShape(12.dp))
            .onSizeChanged { boxSize = it }
            .pointerInput(enabled) {
                if (!enabled) return@pointerInput
                detectDragGestures(
                    onDragStart = { o ->
                        // Read the camera's stable dims once per
                        // gesture-start; they don't change during a
                        // drag, so capturing here means the rest of
                        // the gesture uses a single coord-space target.
                        val surfaceDims = RiseNative.nativeViewportCameraDimensions()
                        val img = mapToImagePixel(
                            o, boxSizeState.value, frameState.value, surfaceDims,
                            paddingPxState.value,
                        ) ?: return@detectDragGestures
                        RiseNative.nativeViewportPointerDown(img.x.toDouble(), img.y.toDouble())
                    },
                    onDrag = { change, _ ->
                        val surfaceDims = RiseNative.nativeViewportCameraDimensions()
                        val img = mapToImagePixel(
                            change.position, boxSizeState.value, frameState.value, surfaceDims,
                            paddingPxState.value,
                        )
                        if (img != null) {
                            RiseNative.nativeViewportPointerMove(img.x.toDouble(), img.y.toDouble())
                        }
                        change.consume()
                    },
                    onDragEnd = {
                        // We don't have the final position here; pass (0,0).
                        // The controller cares about the up event itself,
                        // not the exact coordinate at release.
                        RiseNative.nativeViewportPointerUp(0.0, 0.0)
                    },
                    onDragCancel = {
                        RiseNative.nativeViewportPointerUp(0.0, 0.0)
                    },
                )
            },
        contentAlignment = Alignment.Center,
    ) {
        if (frame != null) {
            Image(
                bitmap = frame,
                contentDescription = null,
                modifier = Modifier.fillMaxSize().padding(ViewportImagePadding),
                contentScale = ContentScale.Fit,
            )
        } else {
            Text(
                "Render to see the scene",
                color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f),
            )
        }
    }
}

@Composable
private fun ViewportTimelineSlider(
    time: Float,
    timelineMax: Float,
    onScrubBegin: () -> Unit,
    onScrub: (Float) -> Unit,
    onScrubEnd: () -> Unit,
    enabled: Boolean,
) {
    var scrubbing by remember { mutableStateOf(false) }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text("%.2fs".format(time),
            style = MaterialTheme.typography.bodySmall,
            modifier = Modifier.width(56.dp))
        Slider(
            value = time,
            enabled = enabled,
            onValueChange = {
                if (!scrubbing) { scrubbing = true; onScrubBegin() }
                onScrub(it)
            },
            onValueChangeFinished = {
                if (scrubbing) { scrubbing = false; onScrubEnd() }
            },
            valueRange = 0f..timelineMax,
            modifier = Modifier.weight(1f),
        )
        Text("%.2fs".format(timelineMax),
            style = MaterialTheme.typography.bodySmall,
            modifier = Modifier.width(56.dp))
    }
}

/**
 * Right-side accordion: four sections (Cameras / Rasterizer /
 * Objects / Lights), each listing the scene's entities in that
 * category.  Single selection across the whole panel — clicking a
 * row in any section auto-collapses the others, expands this one,
 * and surfaces the selected entity's read-only / editable property
 * rows directly under its list.
 *
 * Mirrors the macOS PropertiesPanel.swift and the Windows
 * ViewportProperties.cpp.  Click-on-image object picking on the C++
 * side routes through the same selection state, so the Objects
 * section auto-expands and the picked row highlights.
 */
@Composable
private fun ViewportAccordionPanel(
    properties: List<ViewportPropertyRow>,
    header: String,
    mode: Int,
    selectionCategory: Int,
    selectionName: String,
    entitiesByCategory: Map<Int, List<String>>,
    activeNameByCategory: Map<Int, String>,
    enabled: Boolean,
    onSelectionChanged: (Int, String) -> Unit,
    onPropertyEdited: (String, String) -> Unit,
) {
    Column(Modifier.fillMaxSize().padding(8.dp)) {
        Text(if (header.isEmpty()) "Scene" else header,
            style = MaterialTheme.typography.titleSmall,
            modifier = Modifier.padding(bottom = 4.dp))
        Divider()

        LazyColumn(modifier = Modifier.weight(1f).padding(top = 4.dp)) {
            items(kAccordionSections, key = { it.category }) { section ->
                // Prefer the user's explicit pick when present;
                // otherwise fall back to the scene's active entity
                // (Camera = active camera, Rasterizer = active
                // rasterizer, Object/Light = empty).  This way the
                // dropdown shows the active entity on first load
                // instead of "(pick one)".
                val resolvedName: String =
                    if (selectionCategory == section.category && selectionName.isNotEmpty())
                        selectionName
                    else
                        (activeNameByCategory[section.category] ?: "")
                AccordionSection(
                    section = section,
                    entities = entitiesByCategory[section.category] ?: emptyList(),
                    isExpanded = (selectionCategory == section.category),
                    selectedName = resolvedName,
                    enabled = enabled,
                    onToggle = { open ->
                        if (open) {
                            // Empty-name selection opens the section without
                            // picking a row.  Triggers single-section-open
                            // behavior on the C++ side.
                            onSelectionChanged(section.category, "")
                        } else if (selectionCategory == section.category) {
                            // Collapsing the active section clears selection.
                            onSelectionChanged(0, "")
                        }
                    },
                    onSelectRow = { name ->
                        onSelectionChanged(section.category, name)
                    },
                    propertyRows = if (selectionCategory == section.category && mode != 0) properties else emptyList(),
                    onPropertyEdited = onPropertyEdited,
                )
            }
        }
    }
}

private data class AccordionSectionDef(
    val category: Int,
    val title: String,
)

private val kAccordionSections = listOf(
    AccordionSectionDef(category = 1, title = "Cameras"),
    AccordionSectionDef(category = 2, title = "Rasterizer"),
    AccordionSectionDef(category = 3, title = "Objects"),
    AccordionSectionDef(category = 4, title = "Lights"),
)

@Composable
private fun AccordionSection(
    section: AccordionSectionDef,
    entities: List<String>,
    isExpanded: Boolean,
    selectedName: String,
    enabled: Boolean,
    onToggle: (Boolean) -> Unit,
    onSelectRow: (String) -> Unit,
    propertyRows: List<ViewportPropertyRow>,
    onPropertyEdited: (String, String) -> Unit,
) {
    Column(Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
        // Tap anywhere on the header strip to toggle.  Single hit
        // target so users don't have to aim for the chevron.
        Surface(
            tonalElevation = if (isExpanded) 2.dp else 0.dp,
            shape = RoundedCornerShape(4.dp),
            color = if (isExpanded) MaterialTheme.colorScheme.secondaryContainer
                    else            MaterialTheme.colorScheme.surface,
            modifier = Modifier.fillMaxWidth(),
            onClick = { onToggle(!isExpanded) },
            enabled = enabled,
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(
                    imageVector = if (isExpanded) Icons.Default.KeyboardArrowDown
                                  else            Icons.Default.KeyboardArrowRight,
                    contentDescription = if (isExpanded) "Collapse" else "Expand",
                )
                Spacer(Modifier.width(4.dp))
                Text(
                    section.title,
                    style = MaterialTheme.typography.titleSmall,
                    modifier = Modifier.weight(1f),
                )
            }
        }

        if (isExpanded) {
            // Entity selector — every section uses the dropdown for
            // visual consistency.  Objects in particular can run into
            // hundreds of entries which makes an inline list unusable
            // on a tablet form factor; the rest match for parity.
            if (entities.isEmpty()) {
                Text(
                    "(none)",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f),
                    modifier = Modifier.padding(start = 16.dp, top = 4.dp, bottom = 4.dp),
                )
            } else {
                EntityDropdown(
                    entities = entities,
                    selectedName = selectedName,
                    enabled = enabled,
                    onSelect = onSelectRow,
                )
            }

            // Property rows (only the selected entity's, only under
            // its own section)
            if (selectedName.isNotEmpty()) {
                Column(Modifier.padding(start = 12.dp, top = 6.dp, bottom = 6.dp)) {
                    propertyRows.forEach { row ->
                        ViewportPropertyEntry(row, enabled, onPropertyEdited)
                    }
                    if (propertyRows.isEmpty()) {
                        Text(
                            "No properties available.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f),
                        )
                    }
                }
            }
        }
    }
}

/**
 * Dropdown picker for sections (Rasterizer / Object) that can hold
 * many entries.  Uses the same Box + DropdownMenu pattern the
 * property-row preset menus use, so the visual style stays
 * consistent.  Picking an item routes through `onSelect` exactly
 * as a list-row click did, so the rest of the selection plumbing
 * is untouched.
 */
@Composable
private fun EntityDropdown(
    entities: List<String>,
    selectedName: String,
    enabled: Boolean,
    onSelect: (String) -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }
    Box(
        Modifier
            .padding(start = 12.dp, top = 4.dp, end = 4.dp)
            .fillMaxWidth(),
    ) {
        OutlinedButton(
            onClick = { expanded = true },
            enabled = enabled,
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(4.dp),
        ) {
            Text(
                if (selectedName.isEmpty()) "(pick one)" else selectedName,
                style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.weight(1f),
                maxLines = 1,
            )
            Icon(
                imageVector = Icons.Default.ArrowDropDown,
                contentDescription = null,
            )
        }
        DropdownMenu(
            expanded = expanded,
            onDismissRequest = { expanded = false },
        ) {
            entities.forEach { name ->
                DropdownMenuItem(
                    text = { Text(name) },
                    onClick = {
                        expanded = false
                        if (name != selectedName) onSelect(name)
                    },
                )
            }
        }
    }
}

@Composable
private fun ViewportPropertyEntry(
    row: ViewportPropertyRow,
    enabled: Boolean,
    onPropertyEdited: (String, String) -> Unit,
) {
    var text by rememberSaveable(row.name) { mutableStateOf(row.value) }

    // Reflect external (drag-driven) updates without overwriting active edits.
    LaunchedEffect(row.value) { text = row.value }

    Column(Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(row.name,
                style = MaterialTheme.typography.labelMedium,
                modifier = Modifier.weight(1f))
            if (!row.editable) {
                Text("read-only",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f))
            }
        }
        if (row.editable) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (isScrubbableKind(row.kind) && enabled) {
                    // Chevron handle: tap-and-drag up/down to change
                    // the value.  Mirrors the macOS/Windows scrub
                    // pattern (SF Symbol chevron.up.chevron.down /
                    // Unicode ↕) so muscle memory carries between
                    // platforms.  On touch this is the ONLY drag
                    // affordance — the text field remains for IME
                    // entry — so an accidental tap on the value
                    // doesn't accidentally start a scrub.
                    ScrubHandle(
                        currentText = { text },
                        name = row.name,
                        kind = row.kind,
                        onScrubBegin = { RiseNative.nativeViewportBeginPropertyScrub() },
                        onScrub = { newValue ->
                            text = newValue
                            onPropertyEdited(row.name, newValue)
                        },
                        onScrubEnd = { RiseNative.nativeViewportEndPropertyScrub() },
                    )
                    Spacer(Modifier.width(4.dp))
                }
                OutlinedTextField(
                    value = text,
                    onValueChange = { text = it },
                    enabled = enabled,
                    singleLine = true,
                    textStyle = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.weight(1f),
                    keyboardOptions = androidx.compose.foundation.text.KeyboardOptions(
                        keyboardType = KeyboardType.Text,
                        imeAction = ImeAction.Done,
                    ),
                    keyboardActions = KeyboardActions(
                        onDone = { onPropertyEdited(row.name, text) }
                    ),
                )
                // Presets dropdown — appears next to the text field
                // when the descriptor declared any quick-pick values.
                // Critical for the multi-camera "active_camera" row:
                // the user picks a camera by name from this menu
                // instead of being forced to type it.  Picking a value
                // routes through onPropertyEdited (which the parent
                // bumps refreshTrigger on), so the panel rows refresh
                // — important when switching between camera types
                // with different property sets (pinhole vs thinlens).
                if (row.presets.isNotEmpty()) {
                    Spacer(Modifier.width(4.dp))
                    var expanded by remember { mutableStateOf(false) }
                    Box {
                        IconButton(
                            onClick = { expanded = true },
                            enabled = enabled,
                        ) {
                            Icon(
                                Icons.Filled.ArrowDropDown,
                                contentDescription = "Quick-pick presets",
                            )
                        }
                        DropdownMenu(
                            expanded = expanded,
                            onDismissRequest = { expanded = false },
                        ) {
                            row.presets.forEach { preset ->
                                DropdownMenuItem(
                                    text = { Text(preset.label) },
                                    onClick = {
                                        expanded = false
                                        text = preset.value
                                        onPropertyEdited(row.name, preset.value)
                                    },
                                )
                            }
                        }
                    }
                }
            }
        } else {
            Text(row.value,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f))
        }
        if (row.description.isNotEmpty()) {
            Text(row.description,
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f),
                modifier = Modifier.padding(top = 2.dp))
        }
    }
}

/**
 * Single-numeric ValueKind values get the scrub handle.  Vector and
 * string-like fields don't — scrubbing a vector is ambiguous and IME
 * entry is the natural input for non-numeric fields.  Mirrors the
 * macOS / Windows isScrubbableKind helpers; kind values come from
 * `RISE::ValueKind` (Bool=0, UInt=1, Double=2, DoubleVec3=3, ...).
 */
private fun isScrubbableKind(kind: Int): Boolean = (kind == 1) || (kind == 2)

/**
 * Angular fields the camera descriptor surfaces.  These get a fixed
 * 0.5°/px rate (matching the Orbit tool's sensitivity); other
 * numeric fields use the proportional rate.  Without this split, a
 * theta=30° row scrubs at 0.15°/px — too sluggish for orbit work.
 */
private fun isAngularField(name: String): Boolean = when (name) {
    "theta", "phi", "fov", "pitch", "yaw", "roll" -> true
    else -> false
}

/**
 * Per-pixel scrub rate.  Angular = 0.5/px (fixed).  Otherwise
 * proportional to magnitude with a 1e-3 floor so values at zero can
 * still scrub off zero.
 */
private fun scrubRate(name: String, value: Double): Double {
    if (isAngularField(name)) return 0.5
    return kotlin.math.max(kotlin.math.abs(value), 1e-3) * 0.005
}

/**
 * Format a scrubbed value back into the same textual form the C++
 * FormatDouble / FormatUInt helpers produce (`%.6g` for doubles,
 * integer for UInt clamped at zero).  Keeping the formatting in
 * lockstep avoids round-trip drift across many small drags.
 */
private fun formatScrubbed(v: Double, kind: Int): String {
    if (kind == 1 /* UInt */) {
        val n = kotlin.math.max(0L, kotlin.math.round(v).toLong())
        return n.toString()
    }
    // Kotlin doesn't have a direct %.6g; java.util.Locale.ROOT keeps
    // the decimal separator stable across user locales (no commas).
    return java.lang.String.format(java.util.Locale.ROOT, "%.6g", v)
}

/**
 * Click-and-drag chevron handle for scrubbing numeric property
 * values.  Drag up = increase, drag down = decrease.  Rate scales
 * with the current value's magnitude so a 50° FOV and a 0.08
 * aperture both feel natural under the same gesture.
 *
 * Touch is the primary input modality on Android, so the chevron
 * doubles as a tap target — accidental drags on the text field
 * itself don't trigger a scrub, only drags that start on the icon do.
 */
@Composable
private fun ScrubHandle(
    currentText: () -> String,
    name: String,
    kind: Int,
    onScrubBegin: () -> Unit,
    onScrub: (String) -> Unit,
    onScrubEnd: () -> Unit,
) {
    Box(
        modifier = Modifier
            .size(width = 24.dp, height = 32.dp)
            .background(
                color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.05f),
                shape = RoundedCornerShape(4.dp),
            )
            .pointerInput(name, kind) {
                // detectDragGestures gives us start, drag-delta, and
                // end callbacks.  We track the cumulative drag from
                // the press point rather than re-parsing the field on
                // every drag tick — the field's text is already being
                // updated live by our own `onScrub` callback, so
                // re-parsing would compound rounding error and the
                // scrub would feel "sticky" at fine modifiers.
                var startValue = 0.0
                var cumulativeDy = 0f
                detectDragGestures(
                    onDragStart = {
                        startValue = currentText().toDoubleOrNull() ?: 0.0
                        cumulativeDy = 0f
                        // Bracket the scrub gesture so the controller
                        // bumps preview-scale — without this signal,
                        // every kick cancels the in-flight render
                        // before the outer tiles get a chance and the
                        // image only updates in the centre.
                        onScrubBegin()
                    },
                    onDragEnd = { onScrubEnd() },
                    onDragCancel = { onScrubEnd() },
                    onDrag = { change, dragAmount ->
                        change.consume()
                        // Compose Y grows downward (top-left origin),
                        // so subtract dy to make drag-up = positive.
                        cumulativeDy -= dragAmount.y
                        // Two regimes — angular fields (theta/phi/
                        // fov/pitch/yaw/roll) get a fixed 0.5°/px to
                        // match the Orbit tool's sensitivity; other
                        // numerics use proportional rate so small and
                        // large values both scrub at sensible speeds.
                        val rate = scrubRate(name, startValue)
                        val newValue = startValue + cumulativeDy * rate
                        onScrub(formatScrubbed(newValue, kind))
                    },
                )
            },
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            imageVector = Icons.Default.UnfoldMore,
            contentDescription = "Drag up/down to change",
            tint = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f),
            modifier = Modifier.size(16.dp),
        )
    }
}

