package com.risegfx.android.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.ArrowForward
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
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
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.border
import androidx.compose.foundation.combinedClickable
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
 * The toolbar uses the Photoshop-style category-slot layout: three
 * slots (Select / Camera / ObjectTransform) backed by [ToolCategory],
 * each multi-tool slot opens a DropdownMenu on long-press to switch
 * sub-tool.  Scrub lives in the bottom timeline bar.
 */
enum class ViewportTool(val rawValue: Int, val label: String, val tooltip: String) {
    Select(0,
        "Select",
        "Select — click an object in the viewport to make it the target of the next edit"),
    TranslateObject(1,
        "Translate",
        "Translate — drag the selected object to move it through the scene"),
    RotateObject(2,
        "Rotate",
        "Rotate — drag to rotate the selected object around its origin"),
    ScaleObject(3,
        "Scale",
        "Scale — drag up/down to scale the selected object"),
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

    /** Photoshop-style toolbar slot membership. */
    val category: ToolCategory
        get() = when (this) {
            Select          -> ToolCategory.Select
            TranslateObject -> ToolCategory.ObjectTransform
            RotateObject    -> ToolCategory.ObjectTransform
            ScaleObject     -> ToolCategory.ObjectTransform
            OrbitCamera     -> ToolCategory.Camera
            PanCamera       -> ToolCategory.Camera
            ZoomCamera      -> ToolCategory.Camera
            RollCamera      -> ToolCategory.Camera
        }
}

/**
 * Photoshop-style toolbar category — the "slot" a tool sits in.
 * Mirrors `RISE::SceneEditController::ToolCategory` /
 * `RISEViewportToolCategory`.  Numeric values are part of the C-API
 * contract.
 */
enum class ToolCategory(val rawValue: Int, val tooltip: String) {
    Select(0,
        "Select — click an object in the viewport to make it the next edit's target"),
    Camera(1,
        "Camera — orbit, pan, zoom, or roll the camera (long-press to switch sub-tool)"),
    ObjectTransform(2,
        "Transform — translate, rotate, or scale the selected object via the gizmo (long-press to switch sub-tool)");

    /** Sub-tools shown in this slot's flyout, top-to-bottom. */
    val subTools: List<ViewportTool>
        get() = when (this) {
            Select          -> listOf(ViewportTool.Select)
            Camera          -> listOf(
                ViewportTool.OrbitCamera, ViewportTool.PanCamera,
                ViewportTool.ZoomCamera,  ViewportTool.RollCamera,
            )
            ObjectTransform -> listOf(
                ViewportTool.TranslateObject, ViewportTool.RotateObject,
                ViewportTool.ScaleObject,
            )
        }

    /** Default sub-tool the slot shows before any user interaction. */
    val defaultSubTool: ViewportTool
        get() = subTools.first()
}

/** One screen-space gizmo handle from the controller's layout.
 *  Mirrors `RISEViewportGizmoHandle` / `ViewportBridge::GizmoHandle`. */
private data class GizmoHandle(
    val kind: Int,
    val axis: Int,
    val screenX: Double,
    val screenY: Double,
    val screenRadius: Double,
)

/** Kind constants from RISE::SceneEditController::GizmoHandle::Kind. */
private object GizmoKind {
    const val AxisArrow        = 0
    const val AxisPlane        = 1
    const val ScreenCenter     = 2
    const val AxisRing         = 3
    const val ScreenRing       = 4
    const val AxisScaleHandle  = 5
    const val UniformScaleCube = 6
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
    /// L5d — invoked when the user picks a format from the Save
    /// dropdown.  Callee saves the production VFS's last-rendered
    /// FrameStore through bridge.saveAs, returns the absolute path
    /// for snackbar display.  See RenderViewModel.saveRendered.
    canSave: Boolean = false,
    onSave: (String) -> Unit = {},
    /// L5e — LDR preview controls.  Exposure is in EV stops, range
    /// [-6, +6]; tone curve is a DISPLAY_TRANSFORM enum int (0-4).
    /// Both setters bypass the rasterizer; immediate refresh.
    viewExposureEV: Double = 0.0,
    onExposureChange: (Double) -> Unit = {},
    viewToneCurve: Int = 2 /* ACES */,
    onToneCurveChange: (Int) -> Unit = {},
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
            // accordion lives over Cameras / Rasterizer / Objects /
            // Lights / Output Settings (Film) — category ints 1..5
            // derived from `kAccordionSections` below so adding a new
            // section is a one-line change.
            val epoch = RiseNative.nativeViewportSceneEpoch()
            val categoryIds = kAccordionSections.map { it.category }.toIntArray()
            if (epoch != lastEpoch) {
                lastEpoch = epoch
                val fresh = mutableMapOf<Int, List<String>>()
                for (cat in categoryIds) {
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
            for (cat in categoryIds) {
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
            // Top: render-state strip with Render/Cancel/Save buttons
            ViewportStateStrip(
                state = state,
                progress = progress,
                elapsedMs = elapsedMs,
                remainingMs = remainingMs,
                interactionEnabled = interactionEnabled,
                onRender = onRender,
                onCancel = onCancel,
                canSave = canSave,
                onSave = onSave,
            )
            // L5e — Exposure slider + Tone Curve dropdown.  Lives
            // ABOVE the canvas (under the state strip), visually
            // distinct from the time slider that lives at the
            // BOTTOM of the canvas with time-formatted readout.
            // Header row tells the user which slider this is at a
            // glance.  Double-tap on the slider track resets to 0
            // (TapGesture overlay; Material3 Slider doesn't expose
            // a built-in reset path).
            Spacer(Modifier.height(4.dp))
            ExposureControlsRow(
                exposureEV = viewExposureEV.toFloat(),
                onExposureChange = { onExposureChange(it.toDouble()) },
                toneCurve = viewToneCurve,
                onToneCurveChange = onToneCurveChange,
                enabled = state !is RenderState.Idle,
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
                        selectedTool = selectedTool,
                        isProductionRendering = (state is RenderState.Rendering),
                        refreshTrigger = refreshTrigger,
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
    canSave: Boolean = false,
    onSave: (String) -> Unit = {},
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
                // L5d — Save Rendered Image button.  Visible only when
                // the production VFS has at least one frame's worth of
                // data (Done / Cancelled).  EXR is the default; PNG /
                // TIFF cover the LDR side.  Tap-and-hold-style dropdown:
                // anchor button shows "Save", a hidden DropdownMenu
                // appears on tap and presents the format choices.
                if (canSave) {
                    var saveExpanded by remember { mutableStateOf(false) }
                    Box {
                        OutlinedButton(onClick = { saveExpanded = true }) { Text("Save…") }
                        DropdownMenu(
                            expanded = saveExpanded,
                            onDismissRequest = { saveExpanded = false },
                        ) {
                            DropdownMenuItem(
                                text = { Text("EXR (HDR)") },
                                onClick = { saveExpanded = false; onSave("EXR") },
                            )
                            DropdownMenuItem(
                                text = { Text("PNG (LDR)") },
                                onClick = { saveExpanded = false; onSave("PNG") },
                            )
                            DropdownMenuItem(
                                text = { Text("TIFF (LDR)") },
                                onClick = { saveExpanded = false; onSave("TIFF") },
                            )
                        }
                    }
                    Spacer(Modifier.width(8.dp))
                }
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
        ToolCategory.values().forEach { cat ->
            CategorySlot(
                category = cat,
                selectedTool = selectedTool,
                onToolSelected = onToolSelected,
                enabled = enabled,
            )
            Spacer(Modifier.width(4.dp))
        }
        Spacer(Modifier.weight(1f))
        // ArrowBack/ArrowForward are guaranteed to exist in Icons.Default
        // across all material-icons versions; Undo/Redo aren't.  Same
        // semantics: backward = undo, forward = redo.
        IconButton(onClick = onUndo, enabled = enabled) {
            Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Undo")
        }
        IconButton(onClick = onRedo, enabled = enabled) {
            Icon(Icons.AutoMirrored.Filled.ArrowForward, contentDescription = "Redo")
        }
    }
}

/**
 * One Photoshop-style toolbar slot.  Tap activates the slot's
 * currently-shown sub-tool (or, if the slot's category is active,
 * its own tool).  Long-press opens a DropdownMenu of the category's
 * sub-tools.  Visually highlighted when its category matches the
 * active tool — mirrors the macOS SlotIcon's accent-coloured
 * background and the Windows QToolButton:checked stylesheet.
 *
 * Single-tool slots (Select) skip the long-press flyout entirely —
 * there are no alternatives to switch between.
 */
@OptIn(androidx.compose.foundation.ExperimentalFoundationApi::class)
@Composable
private fun CategorySlot(
    category: ToolCategory,
    selectedTool: ViewportTool,
    onToolSelected: (ViewportTool) -> Unit,
    enabled: Boolean,
) {
    val hasFlyout = category.subTools.size > 1
    // Slot icon = active sub-tool if the slot's category matches,
    // otherwise per-category last-used from the bridge (falls back to
    // the category default when the bridge has no memory yet).
    val shownTool: ViewportTool = remember(selectedTool, category) {
        if (selectedTool.category == category) {
            selectedTool
        } else {
            val raw = RiseNative.nativeViewportGetLastSubToolForCategory(category.rawValue)
            ViewportTool.values().firstOrNull { it.rawValue == raw }
                ?: category.defaultSubTool
        }
    }
    val isSelected = (selectedTool.category == category)
    var menuOpen by remember { mutableStateOf(false) }

    // Custom Box-based slot rather than `FilterChip` because Material's
    // FilterChip wraps its content in an INTERNAL `Modifier.clickable`
    // that consumes tap-down events at the inner layer — any
    // `combinedClickable` we attach via the outer `modifier` parameter
    // never receives the gesture, so long-press to open the flyout
    // would silently no-op.  Building the chip-like visual ourselves
    // gives us a single combinedClickable that owns BOTH tap (activate
    // the shown sub-tool) and long-press (open the sub-tool flyout).
    val bg = if (isSelected) MaterialTheme.colorScheme.primary else Color.Transparent
    val fg = if (isSelected) MaterialTheme.colorScheme.onPrimary
             else MaterialTheme.colorScheme.onSurface
    val border = if (isSelected) MaterialTheme.colorScheme.primary
                 else MaterialTheme.colorScheme.outline
    Box {
        Box(
            modifier = Modifier
                .background(bg, RoundedCornerShape(8.dp))
                .border(1.dp, border, RoundedCornerShape(8.dp))
                .combinedClickable(
                    enabled = enabled,
                    onClick = { onToolSelected(shownTool) },
                    onLongClick = { if (hasFlyout) menuOpen = true },
                )
                .padding(horizontal = 12.dp, vertical = 6.dp),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                shownTool.label,
                color = fg,
                style = MaterialTheme.typography.labelSmall,
            )
        }
        if (hasFlyout) {
            DropdownMenu(
                expanded = menuOpen,
                onDismissRequest = { menuOpen = false },
            ) {
                for (sub in category.subTools) {
                    DropdownMenuItem(
                        text = { Text(sub.label) },
                        onClick = {
                            menuOpen = false
                            onToolSelected(sub)
                        },
                    )
                }
            }
        }
    }
}

/**
 * Transparent overlay drawn over the rendered frame when an Object-
 * transform tool is active.  Reads handle positions from the native
 * controller (already in widget-Y-DOWN, stable-pixel space — the
 * C++ side rescales for the subsampled preview), maps via the same
 * aspect-fit math the [Image] uses, and draws axis-coloured discs,
 * plane squares, rotation rings, and centre / uniform glyphs.
 *
 * Hidden during production renders (`isProductionRendering = true`)
 * because the cached handles can be stale relative to the production
 * rasterizer's camera state.  Mirrors macOS [ViewportGizmoOverlay].
 */
@Composable
private fun GizmoOverlay(
    modifier: Modifier,
    frame: ImageBitmap?,
    refreshTrigger: Int,
    paddingPx: Int,
) {
    if (frame == null) return
    var handles by remember { mutableStateOf(emptyList<GizmoHandle>()) }
    var dragActive by remember { mutableStateOf(false) }
    var activeKind by remember { mutableStateOf(-1) }
    var activeAxis by remember { mutableStateOf(-1) }

    LaunchedEffect(refreshTrigger) {
        RiseNative.nativeViewportRefreshGizmoHandles()
        val n = RiseNative.nativeViewportGizmoHandleCount()
        handles = (0 until n).mapNotNull { i ->
            val a = RiseNative.nativeViewportGizmoHandle(i)
            if (a.size != 5) null
            else GizmoHandle(
                kind = a[0].toInt(),
                axis = a[1].toInt(),
                screenX = a[2],
                screenY = a[3],
                screenRadius = a[4],
            )
        }
        dragActive = RiseNative.nativeViewportIsGizmoDragActive()
        activeKind = RiseNative.nativeViewportActiveGizmoKind()
        activeAxis = RiseNative.nativeViewportActiveGizmoAxis()
    }

    Canvas(modifier = modifier) {
        if (handles.isEmpty()) return@Canvas
        // Pull the camera's stable surface dims, packed (w in hi 32,
        // h in lo 32) by [nativeViewportCameraDimensions].
        val packed = RiseNative.nativeViewportCameraDimensions()
        val surfW = (packed ushr 32).toInt().toFloat()
        val surfH = (packed and 0xFFFFFFFFL).toInt().toFloat()
        if (surfW <= 0f || surfH <= 0f) return@Canvas
        val effW = size.width  - 2 * paddingPx
        val effH = size.height - 2 * paddingPx
        if (effW <= 0f || effH <= 0f) return@Canvas
        val scale = minOf(effW / surfW, effH / surfH)
        val drawW = surfW * scale
        val drawH = surfH * scale
        val ox = paddingPx + (effW - drawW) / 2f
        val oy = paddingPx + (effH - drawH) / 2f

        fun axisColor(axis: Int): Color = when (axis) {
            0    -> Color(0xFFDC3C3C)  // X red
            1    -> Color(0xFF50C850)  // Y green
            2    -> Color(0xFF5078E6)  // Z blue
            else -> Color(0xFFE6C83C)  // screen-aligned yellow
        }

        for (h in handles) {
            val cx = ox + h.screenX.toFloat() * scale
            val cy = oy + h.screenY.toFloat() * scale
            val r  = (h.screenRadius.toFloat() * scale).coerceAtLeast(2f)
            val col = axisColor(h.axis)
            val isActive = dragActive && activeKind == h.kind && activeAxis == h.axis
            val strokeC = if (isActive) Color.White else col
            val strokeW = if (isActive) 2.5f else 1.5f
            when (h.kind) {
                GizmoKind.AxisArrow, GizmoKind.AxisScaleHandle -> {
                    drawCircle(color = col.copy(alpha = 0.85f),
                               radius = r, center = androidx.compose.ui.geometry.Offset(cx, cy))
                    drawCircle(color = strokeC, radius = r,
                               center = androidx.compose.ui.geometry.Offset(cx, cy),
                               style = androidx.compose.ui.graphics.drawscope.Stroke(width = strokeW))
                }
                GizmoKind.AxisPlane -> {
                    val s = r * 1.4f
                    drawRect(color = col.copy(alpha = 0.40f),
                             topLeft = androidx.compose.ui.geometry.Offset(cx - s, cy - s),
                             size = androidx.compose.ui.geometry.Size(2 * s, 2 * s))
                    drawRect(color = strokeC,
                             topLeft = androidx.compose.ui.geometry.Offset(cx - s, cy - s),
                             size = androidx.compose.ui.geometry.Size(2 * s, 2 * s),
                             style = androidx.compose.ui.graphics.drawscope.Stroke(width = strokeW))
                }
                GizmoKind.ScreenCenter, GizmoKind.UniformScaleCube -> {
                    drawCircle(color = col.copy(alpha = 0.30f),
                               radius = r, center = androidx.compose.ui.geometry.Offset(cx, cy))
                    drawCircle(color = strokeC, radius = r,
                               center = androidx.compose.ui.geometry.Offset(cx, cy),
                               style = androidx.compose.ui.graphics.drawscope.Stroke(width = strokeW))
                }
                GizmoKind.AxisRing, GizmoKind.ScreenRing -> {
                    drawCircle(color = strokeC.copy(alpha = if (isActive) 1.0f else 0.8f),
                               radius = r, center = androidx.compose.ui.geometry.Offset(cx, cy),
                               style = androidx.compose.ui.graphics.drawscope.Stroke(
                                   width = if (isActive) 3.0f else 2.0f))
                }
            }
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
    selectedTool: ViewportTool = ViewportTool.Select,
    isProductionRendering: Boolean = false,
    refreshTrigger: Int = 0,
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
                // Tap-only gesture detector — for the Select tool's
                // pick path (controller's `OnPointerDown` runs
                // `PickAt(px)` which casts a ray and sets the Object
                // selection on hit).  `detectDragGestures` BELOW only
                // fires after the user crosses the touch-slop
                // threshold, so a clean tap (down → up without moving
                // past slop) NEVER reaches the controller without
                // this detector.  Tap is implemented as a fused
                // PointerDown + PointerUp at the same image-pixel
                // coordinate so the Select tool's down-handler runs
                // exactly the same way a real touch-and-release does.
                //
                // Compose's gesture coordination keeps tap and drag
                // mutually exclusive: a real drag (move > slop)
                // cancels the tap, and a real tap (no move within the
                // timeout) doesn't trigger drag.  So adding this
                // parallel detector is safe — neither path fires when
                // the other should.
                if (!enabled) return@pointerInput
                detectTapGestures(
                    onTap = { o ->
                        val surfaceDims = RiseNative.nativeViewportCameraDimensions()
                        val img = mapToImagePixel(
                            o, boxSizeState.value, frameState.value, surfaceDims,
                            paddingPxState.value,
                        ) ?: return@detectTapGestures
                        RiseNative.nativeViewportPointerDown(img.x.toDouble(), img.y.toDouble())
                        RiseNative.nativeViewportPointerUp(img.x.toDouble(), img.y.toDouble())
                    },
                )
            }
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
            // Gizmo overlay — drawn on top of the rendered frame
            // when an Object-transform tool is active AND we're not
            // in the middle of a production render.  Mirrors macOS
            // ViewportGizmoOverlay's gating logic.  The Canvas
            // doesn't consume pointer events so the underlying
            // drag-detection on the Box continues to drive the
            // controller; the controller's own hit-test against
            // the cached handles routes drags to the right gizmo
            // handler.
            if (selectedTool.category == ToolCategory.ObjectTransform
                && !isProductionRendering) {
                GizmoOverlay(
                    modifier = Modifier.fillMaxSize(),
                    frame = frame,
                    refreshTrigger = refreshTrigger,
                    paddingPx = paddingPx,
                )
            }
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
        HorizontalDivider()

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
    // category = 6 (Material) intentionally omitted — separate gap,
    // not in scope for the media UX surfacing.
    AccordionSectionDef(category = 7, title = "Media"),
    AccordionSectionDef(category = 5, title = "Output Settings"),
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
                                  else            Icons.AutoMirrored.Filled.KeyboardArrowRight,
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

// L5e — Exposure slider + Tone Curve dropdown.  Compact row above
// the canvas.  Visually distinct from the bottom time slider:
//   * Header row labels "Exposure" with EV-formatted value
//     ("+1.2 EV", "0.0 EV", "-3.5 EV") — time slider uses time
//     formatting ("2.45 s") and lives below the canvas.
//   * Tone curve dropdown sits next to the slider.
//
// **Reset to 0 EV**: tap the EV-readout text in the header row.
// An earlier round used `Modifier.pointerInput { detectTapGestures(
// onDoubleTap = …) }` stacked atop the Slider as a double-tap
// reset target, but that approach broke the slider entirely:
// `detectTapGestures` reads pointer-down events from the channel
// during its tap-vs-drag disambiguation — once the overlay's
// coroutine consumes the initial DOWN, the Slider underneath
// misses it and never starts dragging.  Even after the gesture
// detector exits without claiming the drag, the early-channel-
// read damage is done.  Tapping the EV value label is the more
// reliable Compose pattern AND a wider, more discoverable tap
// target on touch devices.
@Composable
private fun ExposureControlsRow(
    exposureEV: Float,
    onExposureChange: (Float) -> Unit,
    toneCurve: Int,
    onToneCurveChange: (Int) -> Unit,
    enabled: Boolean,
) {
    Column(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(
                imageVector = Icons.Default.WbSunny,
                contentDescription = null,
                modifier = Modifier.size(16.dp),
                tint = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f),
            )
            Spacer(Modifier.width(4.dp))
            Text(
                "Exposure",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.8f),
            )
            Spacer(Modifier.weight(1f))

            // EV readout doubles as a tap-to-reset target.  TextButton
            // gives ripple feedback + Material's recommended min-tap-
            // target sizing; compact contentPadding keeps it from
            // dominating the row.  Auto-disabled when already at 0 EV
            // so the affordance only lights up when there's something
            // to reset (also makes "tap to reset" intent obvious to
            // users who haven't read the tooltip).
            TextButton(
                onClick = { onExposureChange(0f) },
                enabled = enabled && exposureEV != 0f,
                contentPadding = PaddingValues(horizontal = 8.dp, vertical = 0.dp),
                modifier = Modifier.heightIn(min = 32.dp),
            ) {
                Text(
                    formatEV(exposureEV),
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            Spacer(Modifier.width(4.dp))

            // Tone curve dropdown.  Compact button labelled with
            // current curve; tap to open menu.  Greyed out via
            // `enabled` (caller passes false in idle state).
            var tcExpanded by remember { mutableStateOf(false) }
            Box {
                OutlinedButton(
                    onClick = { tcExpanded = true },
                    enabled = enabled,
                    modifier = Modifier.heightIn(min = 32.dp),
                ) {
                    Text(toneCurveLabel(toneCurve),
                         style = MaterialTheme.typography.bodySmall)
                }
                DropdownMenu(
                    expanded = tcExpanded,
                    onDismissRequest = { tcExpanded = false },
                ) {
                    val labels = listOf("None", "Reinhard", "ACES", "AgX", "Hable")
                    labels.forEachIndexed { idx, label ->
                        DropdownMenuItem(
                            text = { Text(label) },
                            onClick = {
                                tcExpanded = false
                                onToneCurveChange(idx)
                            },
                            trailingIcon = if (idx == toneCurve) {
                                { Icon(Icons.Default.Check, contentDescription = null,
                                       modifier = Modifier.size(16.dp)) }
                            } else null,
                        )
                    }
                }
            }
        }

        // Bare slider, no overlay — the EV readout above is the
        // reset affordance.  Pointer-input pipeline stays
        // unobstructed so drag events land cleanly.
        Slider(
            value = exposureEV.coerceIn(-6f, 6f),
            onValueChange = { onExposureChange(it) },
            valueRange = -6f..6f,
            enabled = enabled,
            modifier = Modifier.fillMaxWidth(),
        )
    }
}

private fun formatEV(v: Float): String {
    val sign = if (v > 0f) "+" else ""
    return String.format("%s%.1f EV", sign, v)
}

private fun toneCurveLabel(curve: Int): String = when (curve) {
    0 -> "None"
    1 -> "Reinhard"
    2 -> "ACES"
    3 -> "AgX"
    4 -> "Hable"
    else -> "?"
}

