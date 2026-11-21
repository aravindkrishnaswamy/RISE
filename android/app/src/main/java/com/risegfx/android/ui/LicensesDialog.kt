package com.risegfx.android.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Asset written by the `stageThirdPartyNotices` Gradle task
 * (android/app/build.gradle.kts): every third-party licence and copyright
 * notice the app must carry, concatenated.  The per-package files sit
 * beside it under `licenses/`.
 */
const val THIRD_PARTY_NOTICES_ASSET = "licenses/THIRD-PARTY-NOTICES.txt"

/** Full-screen, scrollable view of [THIRD_PARTY_NOTICES_ASSET]. */
@Composable
fun LicensesDialog(onDismiss: () -> Unit) {
    val context = LocalContext.current
    val lines by produceState<List<String>?>(initialValue = null) {
        value = withContext(Dispatchers.IO) {
            runCatching {
                context.assets.open(THIRD_PARTY_NOTICES_ASSET)
                    .bufferedReader(Charsets.UTF_8)
                    .use { it.readLines() }
            }.getOrElse { listOf("Could not read $THIRD_PARTY_NOTICES_ASSET: ${it.message}") }
        }
    }
    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false),
    ) {
        Surface(
            modifier = Modifier.fillMaxSize().padding(16.dp),
            shape = RoundedCornerShape(12.dp),
        ) {
            Column(Modifier.padding(16.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        "Third-party licences",
                        style = MaterialTheme.typography.titleMedium,
                        modifier = Modifier.weight(1f),
                    )
                    TextButton(onClick = onDismiss) { Text("Close") }
                }
                Spacer(Modifier.height(8.dp))
                val loaded = lines
                if (loaded == null) {
                    LinearProgressIndicator(Modifier.fillMaxWidth())
                } else {
                    // One item per line: the LLVM notice alone is thousands of
                    // lines, too long for a single Text.
                    LazyColumn(Modifier.fillMaxSize()) {
                        items(loaded) { line ->
                            Text(
                                line,
                                style = MaterialTheme.typography.bodySmall,
                                fontFamily = FontFamily.Monospace,
                            )
                        }
                    }
                }
            }
        }
    }
}
