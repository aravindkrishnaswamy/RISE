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

JNIF(jlong, nativeSetCallback)(JNIEnv* env, jobject /*thiz*/, jobject kotlinCallback,
                               jlong requestGeneration) {
    return static_cast<jlong>(getBridge().setCallback(
        env,kotlinCallback,static_cast<uint64_t>(requestGeneration)));
}

JNIF(void, nativeClearCallback)(JNIEnv* env, jobject /*thiz*/, jlong ownerToken) {
    getBridge().clearCallback(env, static_cast<uint64_t>(ownerToken));
}

JNIF(jboolean, nativeOwnsCallback)(JNIEnv* /*env*/, jobject /*thiz*/, jlong ownerToken) {
    return getBridge().ownsCallback(static_cast<uint64_t>(ownerToken)) ?
        JNI_TRUE : JNI_FALSE;
}

JNIF(jboolean, nativeLoadScene)(JNIEnv* env, jobject /*thiz*/, jstring jPath,
                                jlong ownerToken) {
    return getBridge().loadScene(jstringToStd(env, jPath),
        static_cast<uint64_t>(ownerToken)) ? JNI_TRUE : JNI_FALSE;
}

JNIF(jboolean, nativeRasterize)(JNIEnv* /*env*/, jobject /*thiz*/, jlong ownerToken) {
    // BLOCKING. Kotlin calls this from Dispatchers.IO. Inside Rasterize(),
    // the library spawns its own pthread worker pool; callbacks fire from
    // those workers and use getJniEnv() to attach-as-daemon.
    return getBridge().rasterize(static_cast<uint64_t>(ownerToken)) ?
        JNI_TRUE : JNI_FALSE;
}

JNIF(jstring, nativeAutoResolvedIntegrator)(JNIEnv* env, jobject /*thiz*/,
                                             jlong ownerToken) {
    return env->NewStringUTF(getBridge().autoResolvedIntegrator(
        static_cast<uint64_t>(ownerToken)).c_str());
}

JNIF(jstring, nativeAutoResolveReason)(JNIEnv* env, jobject /*thiz*/,
                                      jlong ownerToken) {
    return env->NewStringUTF(getBridge().autoResolveReason(
        static_cast<uint64_t>(ownerToken)).c_str());
}

JNIF(jboolean, nativeCancel)(JNIEnv* /*env*/, jobject /*thiz*/, jlong ownerToken) {
    return getBridge().requestCancel(static_cast<uint64_t>(ownerToken)) ?
        JNI_TRUE : JNI_FALSE;
}

JNIF(jboolean, nativeSetSceneTime)(JNIEnv* /*env*/, jobject /*thiz*/, jdouble t,
                                   jlong ownerToken) {
    // Full SetSceneTime — advances the animator AND regenerates every
    // populated photon map at time `t`.  Called by RenderViewModel
    // before nativeRasterize so post-scrub renders pick up caustics
    // consistent with the scrubbed scene state.
    return getBridge().setSceneTime(static_cast<double>(t),
        static_cast<uint64_t>(ownerToken)) ? JNI_TRUE : JNI_FALSE;
}

JNIF(jboolean, nativeHasAnimatedObjects)(JNIEnv* /*env*/, jobject /*thiz*/,
                                         jlong ownerToken) {
    // Whether the loaded scene declares any keyframed objects.  Used
    // by RenderViewModel after nativeLoadScene to drive _hasAnimation
    // — works without the viewport controller being running, unlike
    // the controller-scoped nativeViewportAnimation* getters.
    return getBridge().hasAnimatedObjects(static_cast<uint64_t>(ownerToken)) ?
        JNI_TRUE : JNI_FALSE;
}

// -----------------------------------------------------------------------------
// L4d — live exposure scrubbing & multi-format Save-As
// -----------------------------------------------------------------------------
// The view-EV slider in Compose calls nativeSetViewExposureEV on every
// drag tick; the bridge re-RenderToBuffer's the cached HDR FrameStore
// at the new EV without re-running the rasterizer (instant repaint).
// nativeSaveAs encodes the cached FrameStore via the L2 IFrameEncoder
// pipeline to any registered format ("PNG"/"EXR"/"TIFF"/"HDR"/"RGBEA"/
// "TGA"/"PPM" — case-insensitive) at the Save-As menu pick.
JNIF(void, nativeSetViewExposureEV)(JNIEnv* /*env*/, jobject /*thiz*/,
                                    jdouble ev) {
    getBridge().setViewExposureEV(static_cast<double>(ev));
}

// L5e — LDR view tone curve.  See RiseBridge::setViewToneCurve for
// the int → DISPLAY_TRANSFORM mapping (0 None / 1 Reinhard /
// 2 ACES default / 3 AgX / 4 Hable).
JNIF(void, nativeSetViewToneCurve)(JNIEnv* /*env*/, jobject /*thiz*/,
                                   jint curve) {
    getBridge().setViewToneCurve(static_cast<int>(curve));
}

// L8 round 9 — generation-gated progressive-update poll.  Called by the
// RenderViewModel's 30 Hz coroutine during an active render. Snapshots the
// production VFS chain and reads its FrameStore generation; no-ops if
// the counter hasn't advanced since the last poll, otherwise emits one
// full-image refresh via the standard `onRegionInvalidated` JNI
// path.  Workers fire NO synchronous bridge callbacks per tile;
// they just bump the generation counter in `FrameStore::EndTile`.
// See `RiseBridge::pollProductionVFS` impl + `RISEBridge.mm`
// `ViewportFrameStoreCallbacks::PollAndEmitIfDirty` (architecture
// spec) for the full rationale.
JNIF(void, nativePollProductionVFS)(JNIEnv* /*env*/, jobject /*thiz*/) {
    getBridge().pollProductionVFS();
}

JNIF(jboolean, nativeSaveAs)(JNIEnv* env, jobject /*thiz*/,
                             jstring jPath, jstring jFormat, jdouble ev) {
    return getBridge().saveAs(jstringToStd(env, jPath),
                              jstringToStd(env, jFormat),
                              static_cast<double>(ev))
           ? JNI_TRUE : JNI_FALSE;
}

JNIF(jdouble, nativeViewportLastSceneTime)(JNIEnv* /*env*/, jobject /*thiz*/,
                                           jlong ownerToken) {
    // Canonical scene time tracked by the SceneEditController.  Used
    // by RenderViewModel just before nativeSetSceneTime so the
    // production handoff uses the truth rather than the slider's
    // local copy (which goes stale across undo/redo).  Returns 0
    // when no controller is attached.
    return static_cast<jdouble>(getBridge().viewportLastSceneTime(
        static_cast<uint64_t>(ownerToken)));
}

JNIF(jobject, nativeCopyFramebuffer)(JNIEnv* env, jobject /*thiz*/, jobject destination) {
    return getBridge().copyFramebufferSnapshot(env, destination);
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
                                    jboolean suppressFirstFrame, jlong ownerToken) {
    return getBridge().startViewport(suppressFirstFrame == JNI_TRUE,
        static_cast<uint64_t>(ownerToken)) ? JNI_TRUE : JNI_FALSE;
}
JNIF(jboolean, nativeScaleFilmToFit)(JNIEnv* /*env*/, jobject /*thiz*/,
                                     jint surfaceW, jint surfaceH, jint maxLongEdge,
                                     jlong ownerToken) {
    if (surfaceW <= 0 || surfaceH <= 0 || maxLongEdge <= 0) return JNI_FALSE;
    return getBridge().scaleFilmToFit(
        static_cast<unsigned int>(surfaceW),
        static_cast<unsigned int>(surfaceH),
        static_cast<unsigned int>(maxLongEdge),
        static_cast<uint64_t>(ownerToken)) ? JNI_TRUE : JNI_FALSE;
}
JNIF(jboolean, nativeViewportStop)(JNIEnv* /*env*/, jobject /*thiz*/, jlong ownerToken) {
    return getBridge().stopViewport(static_cast<uint64_t>(ownerToken)) ?
        JNI_TRUE : JNI_FALSE;
}
JNIF(jboolean, nativeViewportIsRunning)(JNIEnv* /*env*/, jobject /*thiz*/,
                                        jlong ownerToken) {
    return getBridge().isViewportRunning(static_cast<uint64_t>(ownerToken)) ?
        JNI_TRUE : JNI_FALSE;
}
JNIF(jboolean, nativeViewportHasLivePreview)(JNIEnv* /*env*/, jobject /*thiz*/,
                                             jlong ownerToken) {
    return getBridge().hasLivePreview(static_cast<uint64_t>(ownerToken)) ?
        JNI_TRUE : JNI_FALSE;
}
JNIF(void, nativeViewportSuppressNextFrame)(JNIEnv* /*env*/, jobject /*thiz*/,
                                             jlong ownerToken) {
    getBridge().viewportSuppressNextFrame(static_cast<uint64_t>(ownerToken));
}
JNIF(void, nativeViewportSetTool)(JNIEnv* /*env*/, jobject /*thiz*/, jint tool,
                                  jlong ownerToken) {
    getBridge().viewportSetTool(static_cast<int>(tool),
        static_cast<uint64_t>(ownerToken));
}
JNIF(jint, nativeViewportCurrentTool)(JNIEnv* /*env*/, jobject /*thiz*/,
                                      jlong ownerToken) {
    return static_cast<jint>(getBridge().viewportCurrentTool(
        static_cast<uint64_t>(ownerToken)));
}
JNIF(jint, nativeViewportCategoryForTool)(JNIEnv* /*env*/, jobject /*thiz*/, jint tool) {
    return static_cast<jint>(getBridge().viewportCategoryForTool(static_cast<int>(tool)));
}
JNIF(jint, nativeViewportDefaultSubToolForCategory)(JNIEnv* /*env*/, jobject /*thiz*/, jint category) {
    return static_cast<jint>(getBridge().viewportDefaultSubToolForCategory(static_cast<int>(category)));
}
JNIF(jint, nativeViewportGetLastSubToolForCategory)(JNIEnv* /*env*/, jobject /*thiz*/,
                                                    jint category, jlong ownerToken) {
    return static_cast<jint>(getBridge().viewportGetLastSubToolForCategory(
        static_cast<int>(category),static_cast<uint64_t>(ownerToken)));
}
JNIF(void, nativeViewportRefreshGizmoHandles)(JNIEnv* /*env*/, jobject /*thiz*/,
                                               jlong ownerToken) {
    getBridge().viewportRefreshGizmoHandles(static_cast<uint64_t>(ownerToken));
}
JNIF(jint, nativeViewportGizmoHandleCount)(JNIEnv* /*env*/, jobject /*thiz*/,
                                           jlong ownerToken) {
    return static_cast<jint>(getBridge().viewportGizmoHandleCount(
        static_cast<uint64_t>(ownerToken)));
}
JNIF(jdoubleArray, nativeViewportGizmoHandle)(JNIEnv* env, jobject /*thiz*/,
                                              jint index, jlong ownerToken) {
    double tmp[5] = { 0, 0, 0, 0, 0 };
    if (!getBridge().viewportGizmoHandle(static_cast<unsigned int>(index),tmp,
            static_cast<uint64_t>(ownerToken))) {
        return env->NewDoubleArray(0);
    }
    jdoubleArray arr = env->NewDoubleArray(5);
    if (arr) env->SetDoubleArrayRegion(arr, 0, 5, tmp);
    return arr;
}
JNIF(jint, nativeViewportGizmoHandleAt)(JNIEnv* /*env*/, jobject /*thiz*/,
                                        jdouble x, jdouble y, jlong ownerToken) {
    return static_cast<jint>(getBridge().viewportGizmoHandleAt(
        x,y,static_cast<uint64_t>(ownerToken)));
}
JNIF(jboolean, nativeViewportIsGizmoDragActive)(JNIEnv* /*env*/, jobject /*thiz*/,
                                                 jlong ownerToken) {
    return getBridge().viewportIsGizmoDragActive(
        static_cast<uint64_t>(ownerToken)) ? JNI_TRUE : JNI_FALSE;
}
JNIF(jint, nativeViewportActiveGizmoKind)(JNIEnv* /*env*/, jobject /*thiz*/,
                                          jlong ownerToken) {
    return static_cast<jint>(getBridge().viewportActiveGizmoKind(
        static_cast<uint64_t>(ownerToken)));
}
JNIF(jint, nativeViewportActiveGizmoAxis)(JNIEnv* /*env*/, jobject /*thiz*/,
                                          jlong ownerToken) {
    return static_cast<jint>(getBridge().viewportActiveGizmoAxis(
        static_cast<uint64_t>(ownerToken)));
}
JNIF(void, nativeViewportPointerDown)(JNIEnv* /*env*/, jobject /*thiz*/,
                                      jdouble x, jdouble y, jlong ownerToken) {
    getBridge().viewportPointerDown(x,y,static_cast<uint64_t>(ownerToken));
}
JNIF(void, nativeViewportPointerMove)(JNIEnv* /*env*/, jobject /*thiz*/,
                                      jdouble x, jdouble y, jlong ownerToken) {
    getBridge().viewportPointerMove(x,y,static_cast<uint64_t>(ownerToken));
}
JNIF(void, nativeViewportPointerUp)(JNIEnv* /*env*/, jobject /*thiz*/,
                                    jdouble x, jdouble y, jlong ownerToken) {
    getBridge().viewportPointerUp(x,y,static_cast<uint64_t>(ownerToken));
}
JNIF(jlong, nativeViewportCameraDimensions)(JNIEnv* /*env*/, jobject /*thiz*/,
                                             jlong ownerToken) {
    // Pack (w, h) into a single jlong so Kotlin can read both
    // dims with one JNI call (cheap on every pointer event).  Hi
    // 32 bits = width, lo 32 bits = height.  Returns 0 when no
    // camera is attached (both halves zero).
    unsigned int w = 0, h = 0;
    getBridge().viewportGetCameraDimensions(
        w,h,static_cast<uint64_t>(ownerToken));
    return (static_cast<jlong>(w) << 32) | static_cast<jlong>(h);
}
JNIF(jboolean, nativeViewportSetSurfaceDimensions)(JNIEnv* /*env*/, jobject /*thiz*/,
                                                    jint width, jint height,
                                                    jlong ownerToken) {
    if (width <= 0 || height <= 0) return JNI_FALSE;
    return getBridge().viewportSetSurfaceDimensions(
        static_cast<unsigned int>(width),static_cast<unsigned int>(height),
        static_cast<uint64_t>(ownerToken))
        ? JNI_TRUE : JNI_FALSE;
}
JNIF(jdouble, nativeViewportAnimationTimeEnd)(JNIEnv* /*env*/, jobject /*thiz*/,
                                               jlong ownerToken) {
    double t0 = 0, t1 = 0;
    unsigned int nf = 0;
    getBridge().viewportGetAnimationOptions(
        t0,t1,nf,static_cast<uint64_t>(ownerToken));
    return t1;
}
JNIF(jint, nativeViewportAnimationNumFrames)(JNIEnv* /*env*/, jobject /*thiz*/,
                                              jlong ownerToken) {
    double t0 = 0, t1 = 0;
    unsigned int nf = 0;
    getBridge().viewportGetAnimationOptions(
        t0,t1,nf,static_cast<uint64_t>(ownerToken));
    return static_cast<jint>(nf);
}
JNIF(jboolean, nativeViewportScrubBegin)(JNIEnv* /*env*/, jobject /*thiz*/,
                                         jlong ownerToken) {
    return getBridge().viewportScrubBegin(static_cast<uint64_t>(ownerToken)) ?
        JNI_TRUE : JNI_FALSE;
}
JNIF(jboolean, nativeViewportScrub)(JNIEnv* /*env*/, jobject /*thiz*/, jdouble t,
                                    jlong ownerToken) {
    return getBridge().viewportScrub(t,static_cast<uint64_t>(ownerToken)) ?
        JNI_TRUE : JNI_FALSE;
}
JNIF(jboolean, nativeViewportScrubEnd)(JNIEnv* /*env*/, jobject /*thiz*/,
                                       jlong ownerToken) {
    return getBridge().viewportScrubEnd(static_cast<uint64_t>(ownerToken)) ?
        JNI_TRUE : JNI_FALSE;
}
JNIF(void, nativeViewportBeginPropertyScrub)(JNIEnv* /*env*/, jobject /*thiz*/,
                                              jlong ownerToken) {
    getBridge().viewportBeginPropertyScrub(static_cast<uint64_t>(ownerToken));
}
JNIF(void, nativeViewportEndPropertyScrub)(JNIEnv* /*env*/, jobject /*thiz*/,
                                            jlong ownerToken) {
    getBridge().viewportEndPropertyScrub(static_cast<uint64_t>(ownerToken));
}
JNIF(void, nativeViewportUndo)(JNIEnv* /*env*/, jobject /*thiz*/, jlong ownerToken) {
    getBridge().viewportUndo(static_cast<uint64_t>(ownerToken));
}
JNIF(void, nativeViewportRedo)(JNIEnv* /*env*/, jobject /*thiz*/, jlong ownerToken) {
    getBridge().viewportRedo(static_cast<uint64_t>(ownerToken));
}
JNIF(jboolean, nativeViewportProductionRender)(JNIEnv* /*env*/, jobject /*thiz*/,
                                                jlong ownerToken) {
    return getBridge().viewportProductionRender(
        static_cast<uint64_t>(ownerToken)) ? JNI_TRUE : JNI_FALSE;
}

JNIF(void, nativeViewportRefreshProperties)(JNIEnv* /*env*/, jobject /*thiz*/,
                                             jlong ownerToken) {
    getBridge().viewportRefreshProperties(static_cast<uint64_t>(ownerToken));
}
JNIF(jint, nativeViewportPanelMode)(JNIEnv* /*env*/, jobject /*thiz*/,
                                    jlong ownerToken) {
    return static_cast<jint>(getBridge().viewportPanelMode(
        static_cast<uint64_t>(ownerToken)));
}
JNIF(jstring, nativeViewportPanelHeader)(JNIEnv* env, jobject /*thiz*/,
                                         jlong ownerToken) {
    return env->NewStringUTF(getBridge().viewportPanelHeader(
        static_cast<uint64_t>(ownerToken)).c_str());
}
JNIF(jint, nativeViewportPropertyCount)(JNIEnv* /*env*/, jobject /*thiz*/,
                                        jlong ownerToken) {
    return static_cast<jint>(getBridge().viewportPropertyCount(
        static_cast<uint64_t>(ownerToken)));
}
JNIF(jstring, nativeViewportPropertyName)(JNIEnv* env, jobject /*thiz*/, jint idx,
                                           jlong ownerToken) {
    return env->NewStringUTF(getBridge().viewportPropertyName(
        static_cast<unsigned>(idx),static_cast<uint64_t>(ownerToken)).c_str());
}
JNIF(jstring, nativeViewportPropertyValue)(JNIEnv* env, jobject /*thiz*/, jint idx,
                                            jlong ownerToken) {
    return env->NewStringUTF(getBridge().viewportPropertyValue(
        static_cast<unsigned>(idx),static_cast<uint64_t>(ownerToken)).c_str());
}
JNIF(jstring, nativeViewportPropertyDescription)(JNIEnv* env, jobject /*thiz*/, jint idx,
                                                  jlong ownerToken) {
    return env->NewStringUTF(getBridge().viewportPropertyDescription(
        static_cast<unsigned>(idx),static_cast<uint64_t>(ownerToken)).c_str());
}
JNIF(jint, nativeViewportPropertyKind)(JNIEnv* /*env*/, jobject /*thiz*/, jint idx,
                                        jlong ownerToken) {
    return static_cast<jint>(getBridge().viewportPropertyKind(
        static_cast<unsigned>(idx),static_cast<uint64_t>(ownerToken)));
}
JNIF(jboolean, nativeViewportPropertyEditable)(JNIEnv* /*env*/, jobject /*thiz*/, jint idx,
                                                jlong ownerToken) {
    return getBridge().viewportPropertyEditable(
        static_cast<unsigned>(idx),static_cast<uint64_t>(ownerToken)) ?
        JNI_TRUE : JNI_FALSE;
}
JNIF(jint, nativeViewportPropertyPresetCount)(JNIEnv* /*env*/, jobject /*thiz*/, jint idx,
                                               jlong ownerToken) {
    return static_cast<jint>(getBridge().viewportPropertyPresetCount(
        static_cast<unsigned>(idx),static_cast<uint64_t>(ownerToken)));
}
JNIF(jstring, nativeViewportPropertyPresetLabel)(JNIEnv* env, jobject /*thiz*/, jint idx,
                                                  jint presetIdx, jlong ownerToken) {
    return env->NewStringUTF(getBridge().viewportPropertyPresetLabel(
        static_cast<unsigned>(idx),static_cast<unsigned>(presetIdx),
        static_cast<uint64_t>(ownerToken)).c_str());
}
JNIF(jstring, nativeViewportPropertyPresetValue)(JNIEnv* env, jobject /*thiz*/, jint idx,
                                                  jint presetIdx, jlong ownerToken) {
    return env->NewStringUTF(getBridge().viewportPropertyPresetValue(
        static_cast<unsigned>(idx),static_cast<unsigned>(presetIdx),
        static_cast<uint64_t>(ownerToken)).c_str());
}
JNIF(jboolean, nativeViewportSetProperty)(JNIEnv* env, jobject /*thiz*/,
                                          jstring jName, jstring jValue,
                                          jlong ownerToken) {
    return getBridge().viewportSetProperty(jstringToStd(env, jName),
        jstringToStd(env,jValue),static_cast<uint64_t>(ownerToken)) ?
        JNI_TRUE : JNI_FALSE;
}

// -----------------------------------------------------------------------------
// Accordion list / selection — Phase 1 right-panel API.
// -----------------------------------------------------------------------------
JNIF(jint, nativeViewportCategoryEntityCount)(JNIEnv* /*env*/, jobject /*thiz*/,
                                               jint category, jlong ownerToken) {
    return static_cast<jint>(getBridge().viewportCategoryEntityCount(
        static_cast<int>(category),static_cast<uint64_t>(ownerToken)));
}
JNIF(jstring, nativeViewportCategoryEntityName)(JNIEnv* env, jobject /*thiz*/, jint category,
                                                 jint idx, jlong ownerToken) {
    return env->NewStringUTF(getBridge().viewportCategoryEntityName(
        static_cast<int>(category),static_cast<unsigned int>(idx),
        static_cast<uint64_t>(ownerToken)).c_str());
}
JNIF(jstring, nativeViewportCategoryActiveName)(JNIEnv* env, jobject /*thiz*/, jint category,
                                                 jlong ownerToken) {
    return env->NewStringUTF(getBridge().viewportCategoryActiveName(
        static_cast<int>(category),static_cast<uint64_t>(ownerToken)).c_str());
}
JNIF(jint, nativeViewportSelectionCategory)(JNIEnv* /*env*/, jobject /*thiz*/,
                                             jlong ownerToken) {
    return static_cast<jint>(getBridge().viewportSelectionCategory(
        static_cast<uint64_t>(ownerToken)));
}
JNIF(jstring, nativeViewportSelectionName)(JNIEnv* env, jobject /*thiz*/,
                                            jlong ownerToken) {
    return env->NewStringUTF(getBridge().viewportSelectionName(
        static_cast<uint64_t>(ownerToken)).c_str());
}
JNIF(jboolean, nativeViewportSetSelection)(JNIEnv* env, jobject /*thiz*/, jint category,
                                            jstring jName, jlong ownerToken) {
    return getBridge().viewportSetSelection(
        static_cast<int>(category),jstringToStd(env,jName),
        static_cast<uint64_t>(ownerToken)) ? JNI_TRUE : JNI_FALSE;
}
JNIF(jint, nativeViewportSceneEpoch)(JNIEnv* /*env*/, jobject /*thiz*/,
                                     jlong ownerToken) {
    return static_cast<jint>(getBridge().viewportSceneEpoch(
        static_cast<uint64_t>(ownerToken)));
}

#undef JNIF

} // extern "C"
