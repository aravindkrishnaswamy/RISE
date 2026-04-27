package com.risegfx.android.ui

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material3.Card
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.windowsizeclass.WindowSizeClass
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.risegfx.android.R
import com.risegfx.android.RiseApplication

/**
 * Top-level screen.  A compact scene-picker dropdown sits in the top
 * bar; below it the loaded scene fills the remaining space (or a
 * "no scene" placeholder when nothing is loaded yet).  Properties
 * for the camera / picked object live INSIDE [ViewportPane], pinned
 * to the canvas's left edge — so the same layout works on tablet
 * landscape, foldable-open, and phone-rotated form factors without
 * any window-size-class branching.
 */
@Composable
fun RenderScreen(
    windowSizeClass: WindowSizeClass,
    viewModel: RenderViewModel,
) {
    val state            by viewModel.state.collectAsState()
    val progress         by viewModel.progress.collectAsState()
    val elapsedMs        by viewModel.elapsedMs.collectAsState()
    val remainingMs      by viewModel.remainingMs.collectAsState()
    val hasAnimation     by viewModel.hasAnimation.collectAsState()
    val sceneLoaded      by viewModel.sceneLoaded.collectAsState()
    val viewportEpoch    by viewModel.viewportEpoch.collectAsState()
    val sceneTimeDouble  by viewModel.sceneTime.collectAsState()
    val frame            = viewModel.frame
    val context     = LocalContext.current
    val riseRoot    = remember { (context.applicationContext as RiseApplication).riseRoot }

    // windowSizeClass is no longer consulted — the new layout (top
    // dropdown + full-bleed viewport with left-pinned properties)
    // works on every form factor without size-class branching.  The
    // parameter is retained so MainActivity's call site doesn't have
    // to change in lockstep.

    // Disable viewport interaction while a render is in flight or scene
    // is loading.  Render → cancel → done lets the viewport take over.
    val interactionEnabled = sceneLoaded
        && state !is RenderState.Loading
        && state !is RenderState.Rendering
        && state !is RenderState.Cancelling

    // Track the currently-selected scene for the dropdown's display
    // text.  Defaults to null (no scene loaded yet).  We don't read
    // this from the viewmodel because loadedFilePath / etc. live
    // there as the absolute path; the dropdown wants the catalog
    // entry's display name.
    var selectedScene by remember { mutableStateOf<SceneEntry?>(null) }

    Surface(
        modifier = Modifier.fillMaxSize(),
        color = MaterialTheme.colorScheme.background,
    ) {
        Column(Modifier.fillMaxSize().padding(12.dp)) {
            ScenePickerBar(
                modifier = Modifier.fillMaxWidth(),
                selected = selectedScene,
                onSceneSelected = { entry ->
                    selectedScene = entry
                    viewModel.loadAndRender(SceneCatalog.absolutePath(riseRoot, entry))
                },
            )
            Spacer(Modifier.height(12.dp))
            if (sceneLoaded) {
                ViewportPane(
                    modifier = Modifier.fillMaxWidth().fillMaxHeight(),
                    frame = frame,
                    hasAnimation = hasAnimation,
                    interactionEnabled = interactionEnabled,
                    state = state,
                    progress = progress,
                    elapsedMs = elapsedMs,
                    remainingMs = remainingMs,
                    onRender = { viewModel.startRender() },
                    onCancel = viewModel::cancel,
                    viewportEpoch = viewportEpoch,
                    sceneTime = sceneTimeDouble.toFloat(),
                    onSceneTimeChange = { viewModel.updateSceneTime(it.toDouble()) },
                )
            } else {
                RenderCanvas(
                    modifier = Modifier.fillMaxWidth().fillMaxHeight(),
                    frame = frame,
                    state = state,
                    progress = progress,
                    elapsedMs = elapsedMs,
                    remainingMs = remainingMs,
                    onCancel = viewModel::cancel,
                )
            }
        }
    }
}

/**
 * Compact top-bar scene picker.  Shows the current scene's display
 * name (or "Pick a scene" placeholder) and expands a dropdown menu
 * with every entry in [SceneCatalog.bundled].  Replaces the prior
 * full-rail [LazyColumn] layout, which ate 240–340dp of horizontal
 * space on tablets — too costly given how rarely the scene list is
 * actually consulted during a session.
 */
@Composable
private fun ScenePickerBar(
    modifier: Modifier = Modifier,
    selected: SceneEntry?,
    onSceneSelected: (SceneEntry) -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }
    Card(modifier, shape = RoundedCornerShape(12.dp)) {
        Box(Modifier.padding(horizontal = 12.dp, vertical = 8.dp)) {
            // Anchor row — clickable to open the menu.  We use a Box
            // so the DropdownMenu's positioning anchors to this exact
            // surface (the menu pops down from its top-left).
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 40.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                OutlinedButton(
                    onClick = { expanded = true },
                    modifier = Modifier.widthIn(min = 200.dp),
                ) {
                    Text(
                        text = selected?.displayName ?: "Pick a scene",
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.Medium,
                    )
                    Spacer(Modifier.width(8.dp))
                    Icon(Icons.Default.ArrowDropDown, contentDescription = "Open scene list")
                }
                if (selected != null) {
                    Spacer(Modifier.width(12.dp))
                    Text(
                        selected.description,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f),
                    )
                }
            }
            DropdownMenu(
                expanded = expanded,
                onDismissRequest = { expanded = false },
                // Cap the width so very long descriptions don't blow
                // the menu out across the whole screen.
                modifier = Modifier.widthIn(min = 280.dp, max = 480.dp),
            ) {
                SceneCatalog.bundled.forEach { entry ->
                    DropdownMenuItem(
                        text = {
                            Column {
                                Text(
                                    entry.displayName,
                                    style = MaterialTheme.typography.bodyLarge,
                                    fontWeight = FontWeight.Medium,
                                )
                                Text(
                                    entry.description,
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f),
                                )
                            }
                        },
                        onClick = {
                            expanded = false
                            onSceneSelected(entry)
                        },
                    )
                }
            }
        }
    }
}

@Composable
private fun RenderCanvas(
    modifier: Modifier,
    frame: ImageBitmap?,
    state: RenderState,
    progress: Float,
    elapsedMs: Long,
    remainingMs: Long?,
    onCancel: () -> Unit,
) {
    Card(modifier, shape = RoundedCornerShape(16.dp)) {
        Column(Modifier.fillMaxSize().padding(16.dp)) {
            StateStrip(state = state, progress = progress, onCancel = onCancel)
            Spacer(Modifier.height(4.dp))
            TimeReadout(state = state, elapsedMs = elapsedMs, remainingMs = remainingMs)
            Spacer(Modifier.height(12.dp))
            Box(
                Modifier
                    .fillMaxWidth()
                    .fillMaxHeight()
                    .background(Color(0xFF101114), RoundedCornerShape(12.dp)),
                contentAlignment = Alignment.Center,
            ) {
                if (frame != null) {
                    Image(
                        bitmap = frame,
                        contentDescription = null,
                        modifier = Modifier.fillMaxSize().padding(8.dp),
                        contentScale = ContentScale.Fit,
                    )
                } else {
                    Text(
                        stringResource(R.string.label_no_scene),
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f),
                    )
                }
            }
        }
    }
}

@Composable
private fun StateStrip(state: RenderState, progress: Float, onCancel: () -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        val label = when (state) {
            is RenderState.Idle        -> "Idle"
            is RenderState.Preparing   -> "Preparing: ${state.reason}"
            is RenderState.Loading     -> "Loading ${state.scenePath.substringAfterLast('/')}…"
            is RenderState.Rendering   -> "Rendering ${state.width}×${state.height} · ${(progress * 100).toInt()}%"
            is RenderState.Cancelling  -> "Cancelling…"
            is RenderState.Done        -> "Done"
            is RenderState.Cancelled   -> "Cancelled"
            is RenderState.Error       -> "Error: ${state.message}"
        }
        Text(label, style = MaterialTheme.typography.titleSmall)
        Spacer(Modifier.weight(1f))
        val canCancel = state is RenderState.Rendering || state is RenderState.Loading
        OutlinedButton(onClick = onCancel, enabled = canCancel) { Text("Cancel") }
    }
    Spacer(Modifier.height(8.dp))
    val showProgress = state is RenderState.Rendering || state is RenderState.Loading || state is RenderState.Cancelling
    if (showProgress) {
        LinearProgressIndicator(
            progress = { progress.coerceIn(0f, 1f) },
            modifier = Modifier.fillMaxWidth().height(6.dp),
        )
    }
}

@Composable
private fun TimeReadout(state: RenderState, elapsedMs: Long, remainingMs: Long?) {
    val isActive = state is RenderState.Rendering || state is RenderState.Cancelling
    val isDone = state is RenderState.Done
    if (!isActive && !isDone) return

    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(
            "Elapsed: ${formatDurationMs(elapsedMs)}",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f),
        )
        Spacer(Modifier.weight(1f))
        if (isActive) {
            val remainingText = remainingMs?.let { "~${formatDurationMs(it)}" } ?: "estimating\u2026"
            Text(
                "Remaining: $remainingText",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f),
            )
        }
    }
}

// Mirror of RISE::RenderETAEstimator::FormatDuration. Kept in Kotlin so the
// UI avoids a JNI round-trip per display tick; the algorithm is trivial.
internal fun formatDurationMs(ms: Long): String {
    val clamped = if (ms < 0L) 0L else ms
    val totalSeconds = (clamped + 500L) / 1000L
    val hours = totalSeconds / 3600L
    val minutes = (totalSeconds % 3600L) / 60L
    val seconds = totalSeconds % 60L
    return if (hours > 0L) {
        String.format("%02d:%02d:%02d", hours, minutes, seconds)
    } else {
        String.format("%d:%02d", minutes, seconds)
    }
}
