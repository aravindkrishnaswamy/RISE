package com.risegfx.android.ui

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.windowsizeclass.WindowSizeClass
import androidx.compose.material3.windowsizeclass.WindowWidthSizeClass
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
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
 * Top-level screen. Adaptive two-pane layout driven by [WindowSizeClass]:
 *   - Expanded (tablet landscape / foldable open): scene list + canvas side by side
 *   - Medium (tablet portrait): narrow scene rail + canvas
 *   - Compact (phone portrait): canvas only with a dropdown for scene selection
 *     — phone support is scoped out for v1, but the fall-through renders a
 *     usable vertical layout so development on a phone emulator still works.
 */
@Composable
fun RenderScreen(
    windowSizeClass: WindowSizeClass,
    viewModel: RenderViewModel,
) {
    val state       by viewModel.state.collectAsState()
    val progress    by viewModel.progress.collectAsState()
    val elapsedMs   by viewModel.elapsedMs.collectAsState()
    val remainingMs by viewModel.remainingMs.collectAsState()
    val frame       = viewModel.frame
    val context     = LocalContext.current
    val scope       = rememberCoroutineScope()
    val riseRoot    = remember { (context.applicationContext as RiseApplication).riseRoot }

    val isExpanded = windowSizeClass.widthSizeClass == WindowWidthSizeClass.Expanded
    val isMedium   = windowSizeClass.widthSizeClass == WindowWidthSizeClass.Medium

    Surface(
        modifier = Modifier.fillMaxSize(),
        color = MaterialTheme.colorScheme.background,
    ) {
        if (isExpanded || isMedium) {
            Row(Modifier.fillMaxSize()) {
                SceneBrowser(
                    modifier = Modifier
                        .widthIn(min = 240.dp, max = 340.dp)
                        .fillMaxHeight()
                        .padding(12.dp),
                    onSceneSelected = { entry ->
                        viewModel.loadAndRender(SceneCatalog.absolutePath(riseRoot, entry))
                    },
                )
                RenderCanvas(
                    modifier = Modifier
                        .fillMaxHeight()
                        .fillMaxWidth()
                        .padding(12.dp),
                    frame = frame,
                    state = state,
                    progress = progress,
                    elapsedMs = elapsedMs,
                    remainingMs = remainingMs,
                    onCancel = viewModel::cancel,
                )
            }
        } else {
            Column(Modifier.fillMaxSize().padding(12.dp)) {
                SceneBrowser(
                    modifier = Modifier.fillMaxWidth().height(200.dp),
                    onSceneSelected = { entry ->
                        viewModel.loadAndRender(SceneCatalog.absolutePath(riseRoot, entry))
                    },
                )
                Spacer(Modifier.height(12.dp))
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

@Composable
private fun SceneBrowser(
    modifier: Modifier,
    onSceneSelected: (SceneEntry) -> Unit,
) {
    Card(modifier, shape = RoundedCornerShape(16.dp)) {
        Column(Modifier.padding(16.dp)) {
            Text("Bundled Scenes", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
            Spacer(Modifier.height(8.dp))
            LazyColumn {
                items(SceneCatalog.bundled) { entry ->
                    SceneRow(entry, onClick = { onSceneSelected(entry) })
                }
            }
        }
    }
}

@Composable
private fun SceneRow(entry: SceneEntry, onClick: () -> Unit) {
    Column(
        Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(vertical = 8.dp),
    ) {
        Text(entry.displayName, style = MaterialTheme.typography.bodyLarge, fontWeight = FontWeight.Medium)
        Text(entry.description, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f))
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
