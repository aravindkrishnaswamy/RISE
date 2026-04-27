// rise_jni.cpp — JNIEXPORT entry points called from
// com.risegfx.android.nativebridge.RiseNative.
//
// This file is the ONLY place that uses JNIEXPORT / JNICALL. Everything else
// lives in RiseBridge and the callback adapters. JNI_OnLoad caches the
// JavaVM and the RiseCallback jmethodIDs, and registers a pthread_key
// destructor so native worker threads detach cleanly on exit.

#include "RiseBridge.h"
#include "jni_util.h"

#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <string>

#define LOG_TAG "RISE-JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using rise_jni::getBridge;
using rise_jni::getJniEnv;

namespace {

// Pull a std::string out of a jstring. Returns empty string for null.
std::string jstringToStd(JNIEnv* env, jstring js) {
    if (!js) return {};
    const char* utf = env->GetStringUTFChars(js, nullptr);
    if (!utf) return {};
    std::string out(utf);
    env->ReleaseStringUTFChars(js, utf);
    return out;
}

} // namespace

extern "C" {

__attribute__((visibility("default")))
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    rise_jni::g_vm = vm;

    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        LOGE("JNI_OnLoad: GetEnv failed");
        return JNI_ERR;
    }

    // The detach key destructor is called for any thread that has called
    // pthread_setspecific with a non-null value. Library worker threads set
    // this in getJniEnv() after AttachCurrentThreadAsDaemon so they detach
    // cleanly when they exit. See jni_util.cpp.
    pthread_key_create(&rise_jni::g_detachKey, [](void*) {
        if (rise_jni::g_vm) {
            rise_jni::g_vm->DetachCurrentThread();
        }
    });

    if (!rise_jni::cacheCallbackIds(env)) {
        LOGE("JNI_OnLoad: cacheCallbackIds failed");
        return JNI_ERR;
    }

    LOGI("JNI_OnLoad: RISE JNI bridge loaded");
    return JNI_VERSION_1_6;
}

// -----------------------------------------------------------------------------
// RiseNative entry points. The Kotlin side declares these as `external fun`
// inside object RiseNative (package com.risegfx.android.nativebridge), so the
// JNI symbol name is Java_com_risegfx_android_nativebridge_RiseNative_<name>.
// -----------------------------------------------------------------------------

#define JNIF(ret, name) \
    __attribute__((visibility("default"))) \
    JNIEXPORT ret JNICALL Java_com_risegfx_android_nativebridge_RiseNative_##name

JNIF(void, nativeInit)(JNIEnv* env, jobject /*thiz*/,
                       jstring jProjectRoot, jstring jLogFile, jint threadCount) {
    getBridge().initialize(jstringToStd(env, jProjectRoot),
                           jstringToStd(env, jLogFile),
                           static_cast<int>(threadCount));
}

JNIF(void, nativeSetCallback)(JNIEnv* env, jobject /*thiz*/, jobject kotlinCallback) {
    getBridge().setCallback(env, kotlinCallback);
}

JNIF(jboolean, nativeLoadScene)(JNIEnv* env, jobject /*thiz*/, jstring jPath) {
    return getBridge().loadScene(jstringToStd(env, jPath)) ? JNI_TRUE : JNI_FALSE;
}

JNIF(jboolean, nativeRasterize)(JNIEnv* /*env*/, jobject /*thiz*/) {
    // BLOCKING. Kotlin calls this from Dispatchers.IO. Inside Rasterize(),
    // the library spawns its own pthread worker pool; callbacks fire from
    // those workers and use getJniEnv() to attach-as-daemon.
    return getBridge().rasterize() ? JNI_TRUE : JNI_FALSE;
}

JNIF(void, nativeCancel)(JNIEnv* /*env*/, jobject /*thiz*/) {
    getBridge().requestCancel();
}

JNIF(void, nativeSetSceneTime)(JNIEnv* /*env*/, jobject /*thiz*/, jdouble t) {
    // Full SetSceneTime — advances the animator AND regenerates every
    // populated photon map at time `t`.  Called by RenderViewModel
    // before nativeRasterize so post-scrub renders pick up caustics
    // consistent with the scrubbed scene state.
    getBridge().setSceneTime(static_cast<double>(t));
}

JNIF(jboolean, nativeHasAnimatedObjects)(JNIEnv* /*env*/, jobject /*thiz*/) {
    // Whether the loaded scene declares any keyframed objects.  Used
    // by RenderViewModel after nativeLoadScene to drive _hasAnimation
    // — works without the viewport controller being running, unlike
    // the controller-scoped nativeViewportAnimation* getters.
    return getBridge().hasAnimatedObjects() ? JNI_TRUE : JNI_FALSE;
}

JNIF(jdouble, nativeViewportLastSceneTime)(JNIEnv* /*env*/, jobject /*thiz*/) {
    // Canonical scene time tracked by the SceneEditController.  Used
    // by RenderViewModel just before nativeSetSceneTime so the
    // production handoff uses the truth rather than the slider's
    // local copy (which goes stale across undo/redo).  Returns 0
    // when no controller is attached.
    return static_cast<jdouble>(getBridge().viewportLastSceneTime());
}

JNIF(jobject, nativeGetFramebuffer)(JNIEnv* env, jobject /*thiz*/) {
    return getBridge().getFramebufferByteBuffer(env);
}

JNIF(void, nativeEtaBegin)(JNIEnv* /*env*/, jobject /*thiz*/) {
    getBridge().etaBegin();
}

JNIF(jlong, nativeEtaElapsedMs)(JNIEnv* /*env*/, jobject /*thiz*/) {
    return static_cast<jlong>(getBridge().etaElapsedMs());
}

// Returns -1 when the estimator is still warming up; the UI should display
// "estimating..." in that case.
JNIF(jlong, nativeEtaRemainingMs)(JNIEnv* /*env*/, jobject /*thiz*/) {
    return static_cast<jlong>(getBridge().etaRemainingMs());
}

// -----------------------------------------------------------------------------
// Interactive viewport
// -----------------------------------------------------------------------------

JNIF(jboolean, nativeViewportStart)(JNIEnv* /*env*/, jobject /*thiz*/,
                                    jboolean suppressFirstFrame) {
    return getBridge().startViewport(suppressFirstFrame == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}
JNIF(void, nativeViewportStop)(JNIEnv* /*env*/, jobject /*thiz*/) {
    getBridge().stopViewport();
}
JNIF(jboolean, nativeViewportIsRunning)(JNIEnv* /*env*/, jobject /*thiz*/) {
    return getBridge().isViewportRunning() ? JNI_TRUE : JNI_FALSE;
}
JNIF(jboolean, nativeViewportHasLivePreview)(JNIEnv* /*env*/, jobject /*thiz*/) {
    return getBridge().hasLivePreview() ? JNI_TRUE : JNI_FALSE;
}
JNIF(void, nativeViewportSuppressNextFrame)(JNIEnv* /*env*/, jobject /*thiz*/) {
    getBridge().viewportSuppressNextFrame();
}
JNIF(void, nativeViewportSetTool)(JNIEnv* /*env*/, jobject /*thiz*/, jint tool) {
    getBridge().viewportSetTool(static_cast<int>(tool));
}
JNIF(void, nativeViewportPointerDown)(JNIEnv* /*env*/, jobject /*thiz*/, jdouble x, jdouble y) {
    getBridge().viewportPointerDown(x, y);
}
JNIF(void, nativeViewportPointerMove)(JNIEnv* /*env*/, jobject /*thiz*/, jdouble x, jdouble y) {
    getBridge().viewportPointerMove(x, y);
}
JNIF(void, nativeViewportPointerUp)(JNIEnv* /*env*/, jobject /*thiz*/, jdouble x, jdouble y) {
    getBridge().viewportPointerUp(x, y);
}
JNIF(jlong, nativeViewportCameraDimensions)(JNIEnv* /*env*/, jobject /*thiz*/) {
    // Pack (w, h) into a single jlong so Kotlin can read both
    // dims with one JNI call (cheap on every pointer event).  Hi
    // 32 bits = width, lo 32 bits = height.  Returns 0 when no
    // camera is attached (both halves zero).
    unsigned int w = 0, h = 0;
    getBridge().viewportGetCameraDimensions(w, h);
    return (static_cast<jlong>(w) << 32) | static_cast<jlong>(h);
}
JNIF(jdouble, nativeViewportAnimationTimeEnd)(JNIEnv* /*env*/, jobject /*thiz*/) {
    double t0 = 0, t1 = 0;
    unsigned int nf = 0;
    getBridge().viewportGetAnimationOptions(t0, t1, nf);
    return t1;
}
JNIF(jint, nativeViewportAnimationNumFrames)(JNIEnv* /*env*/, jobject /*thiz*/) {
    double t0 = 0, t1 = 0;
    unsigned int nf = 0;
    getBridge().viewportGetAnimationOptions(t0, t1, nf);
    return static_cast<jint>(nf);
}
JNIF(void, nativeViewportScrubBegin)(JNIEnv* /*env*/, jobject /*thiz*/) {
    getBridge().viewportScrubBegin();
}
JNIF(void, nativeViewportScrub)(JNIEnv* /*env*/, jobject /*thiz*/, jdouble t) {
    getBridge().viewportScrub(t);
}
JNIF(void, nativeViewportScrubEnd)(JNIEnv* /*env*/, jobject /*thiz*/) {
    getBridge().viewportScrubEnd();
}
JNIF(void, nativeViewportBeginPropertyScrub)(JNIEnv* /*env*/, jobject /*thiz*/) {
    getBridge().viewportBeginPropertyScrub();
}
JNIF(void, nativeViewportEndPropertyScrub)(JNIEnv* /*env*/, jobject /*thiz*/) {
    getBridge().viewportEndPropertyScrub();
}
JNIF(void, nativeViewportUndo)(JNIEnv* /*env*/, jobject /*thiz*/) {
    getBridge().viewportUndo();
}
JNIF(void, nativeViewportRedo)(JNIEnv* /*env*/, jobject /*thiz*/) {
    getBridge().viewportRedo();
}
JNIF(jboolean, nativeViewportProductionRender)(JNIEnv* /*env*/, jobject /*thiz*/) {
    return getBridge().viewportProductionRender() ? JNI_TRUE : JNI_FALSE;
}

JNIF(void, nativeViewportRefreshProperties)(JNIEnv* /*env*/, jobject /*thiz*/) {
    getBridge().viewportRefreshProperties();
}
JNIF(jint, nativeViewportPanelMode)(JNIEnv* /*env*/, jobject /*thiz*/) {
    return static_cast<jint>(getBridge().viewportPanelMode());
}
JNIF(jstring, nativeViewportPanelHeader)(JNIEnv* env, jobject /*thiz*/) {
    return env->NewStringUTF(getBridge().viewportPanelHeader().c_str());
}
JNIF(jint, nativeViewportPropertyCount)(JNIEnv* /*env*/, jobject /*thiz*/) {
    return static_cast<jint>(getBridge().viewportPropertyCount());
}
JNIF(jstring, nativeViewportPropertyName)(JNIEnv* env, jobject /*thiz*/, jint idx) {
    return env->NewStringUTF(getBridge().viewportPropertyName(static_cast<unsigned>(idx)).c_str());
}
JNIF(jstring, nativeViewportPropertyValue)(JNIEnv* env, jobject /*thiz*/, jint idx) {
    return env->NewStringUTF(getBridge().viewportPropertyValue(static_cast<unsigned>(idx)).c_str());
}
JNIF(jstring, nativeViewportPropertyDescription)(JNIEnv* env, jobject /*thiz*/, jint idx) {
    return env->NewStringUTF(getBridge().viewportPropertyDescription(static_cast<unsigned>(idx)).c_str());
}
JNIF(jint, nativeViewportPropertyKind)(JNIEnv* /*env*/, jobject /*thiz*/, jint idx) {
    return static_cast<jint>(getBridge().viewportPropertyKind(static_cast<unsigned>(idx)));
}
JNIF(jboolean, nativeViewportPropertyEditable)(JNIEnv* /*env*/, jobject /*thiz*/, jint idx) {
    return getBridge().viewportPropertyEditable(static_cast<unsigned>(idx)) ? JNI_TRUE : JNI_FALSE;
}
JNIF(jboolean, nativeViewportSetProperty)(JNIEnv* env, jobject /*thiz*/,
                                          jstring jName, jstring jValue) {
    return getBridge().viewportSetProperty(jstringToStd(env, jName),
                                           jstringToStd(env, jValue)) ? JNI_TRUE : JNI_FALSE;
}

#undef JNIF

} // extern "C"
