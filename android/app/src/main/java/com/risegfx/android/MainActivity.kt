package com.risegfx.android

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.viewModels
import androidx.compose.material3.windowsizeclass.ExperimentalMaterial3WindowSizeClassApi
import androidx.compose.material3.windowsizeclass.calculateWindowSizeClass
import com.risegfx.android.ui.RenderScreen
import com.risegfx.android.ui.RenderViewModel
import com.risegfx.android.ui.theme.RiseTheme

class MainActivity : ComponentActivity() {

    private val viewModel: RenderViewModel by viewModels()

    @OptIn(ExperimentalMaterial3WindowSizeClassApi::class)
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            RiseTheme {
                val sizeClass = calculateWindowSizeClass(this)
                RenderScreen(windowSizeClass = sizeClass, viewModel = viewModel)
            }
        }
    }
}
