// Top-level build file. Version constants live here; module build files pick
// them up via the plugins {} block.
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.kotlin.android) apply false
    alias(libs.plugins.kotlin.compose) apply false
}
