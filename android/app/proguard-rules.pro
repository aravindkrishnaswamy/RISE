# Keep JNI classes & methods — they are called reflectively from native code.
-keep class com.risegfx.android.nativebridge.RiseNative { *; }
-keep class com.risegfx.android.nativebridge.RiseCallback { *; }
-keepclassmembers class com.risegfx.android.nativebridge.** {
    native <methods>;
}
