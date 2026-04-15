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

JNIF(jobject, nativeGetFramebuffer)(JNIEnv* env, jobject /*thiz*/) {
    return getBridge().getFramebufferByteBuffer(env);
}

#undef JNIF

} // extern "C"
