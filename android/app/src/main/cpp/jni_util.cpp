#include "jni_util.h"

#include <android/log.h>

#define LOG_TAG "RISE-JNI"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace rise_jni {

JavaVM*                g_vm        = nullptr;
CachedCallbackMethods  g_cb        = {};
pthread_key_t          g_detachKey = 0;

namespace {
// pthread_key destructor: detach the thread from the JVM when it exits so we
// don't leak JVM-side ThreadLocal state. This runs on each library worker
// thread shutdown (they are spawned by pthread_create inside librise per
// Rasterize() call).
void detachOnThreadExit(void* /*ignored*/) {
    if (g_vm) {
        g_vm->DetachCurrentThread();
    }
}
} // namespace

JNIEnv* getJniEnv() {
    if (!g_vm) {
        LOGE("getJniEnv: g_vm is null (JNI_OnLoad did not run?)");
        return nullptr;
    }

    JNIEnv* env = nullptr;
    jint rc = g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);

    if (rc == JNI_OK) {
        return env;
    }

    if (rc == JNI_EDETACHED) {
        // Attach as daemon so process shutdown doesn't deadlock waiting for
        // this thread to detach. The pthread_key destructor below will detach
        // us when the thread exits normally.
        if (g_vm->AttachCurrentThreadAsDaemon(&env, nullptr) != JNI_OK) {
            LOGE("getJniEnv: AttachCurrentThreadAsDaemon failed");
            return nullptr;
        }
        // Sentinel value — we just need pthread_setspecific to record that
        // this thread has a destructor to run.
        pthread_setspecific(g_detachKey, reinterpret_cast<void*>(1));
        return env;
    }

    LOGE("getJniEnv: unexpected JNI version rc=%d", rc);
    return nullptr;
}

bool cacheCallbackIds(JNIEnv* env) {
    jclass local = env->FindClass("com/risegfx/android/nativebridge/RiseCallback");
    if (!local) {
        LOGE("cacheCallbackIds: could not find RiseCallback class");
        return false;
    }
    g_cb.riseCallbackClass = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);

    g_cb.onProgress          = env->GetMethodID(g_cb.riseCallbackClass, "onProgress",          "(F)V");
    g_cb.onRegionInvalidated = env->GetMethodID(g_cb.riseCallbackClass, "onRegionInvalidated", "(J)V");
    g_cb.onSceneReady        = env->GetMethodID(g_cb.riseCallbackClass, "onSceneReady",        "(II)V");
    g_cb.onLog               = env->GetMethodID(g_cb.riseCallbackClass, "onLog",               "(ILjava/lang/String;)V");

    if (!g_cb.onProgress || !g_cb.onRegionInvalidated || !g_cb.onSceneReady || !g_cb.onLog) {
        LOGE("cacheCallbackIds: missing one or more RiseCallback methods");
        return false;
    }
    return true;
}

} // namespace rise_jni
