package com.risegfx.android.ui

/**
 * High-level render pipeline state, driven by [RenderViewModel]. Modelled
 * after the Mac bridge's internal states: Idle / Loading / Rendering / Done /
 * Cancelled / Error. The UI maps these to progress bars, button enable/disable
 * and status text.
 */
sealed class RenderState {
    data object Idle : RenderState()

    data class Preparing(val reason: String) : RenderState()

    data class Loading(val scenePath: String) : RenderState()

    data class Rendering(val width: Int, val height: Int) : RenderState()

    data object Cancelling : RenderState()

    data object Done : RenderState()

    data object Cancelled : RenderState()

    data class Error(val message: String) : RenderState()
}
