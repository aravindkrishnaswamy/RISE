// jni_util.h — small helpers for the RISE JNI bridge.
//
// Responsibilities:
//   - Cache the JavaVM* and the RiseCallback interface's jmethodIDs at JNI_OnLoad.
//   - Provide a thread-safe JNIEnv* accessor that attaches the calling thread
//     (using AttachCurrentThreadAsDaemon, not plain Attach) and registers a
//     pthread_key destructor so native worker threads detach cleanly on exit.
//   - Provide a scoped JNI local-frame helper to avoid local-ref leaks in
//     tile callbacks, which fire hundreds of times per render.
//
// This header is only usable from the C++ side. Nothing here is exported.

#ifndef RISE_JNI_UTIL_H_
#define RISE_JNI_UTIL_H_

#include <jni.h>
#include <pthread.h>

namespace rise_jni {

// JNI method IDs for the RiseCallback interface. Cached in JNI_OnLoad so the
// hot path (tile callbacks) never does name lookups.
struct CachedCallbackMethods {
    jclass    riseCallbackClass;   // global ref
    jmethodID onProgress;          // void onProgress(float)
    jmethodID onRegionInvalidated; // void onRegionInvalidated(long packedRect)
    jmethodID onSceneReady;        // void onSceneReady(int w, int h)
    jmethodID onLog;               // void onLog(int level, String msg)
};

extern JavaVM*                   g_vm;
extern CachedCallbackMethods     g_cb;
extern pthread_key_t             g_detachKey;

// Attach the calling thread to the JVM if necessary and return a JNIEnv*.
// Uses AttachCurrentThreadAsDaemon so that a lingering worker thread at
// process shutdown does not block the JVM waiting for detach. The detach
// happens automatically via a pthread_key destructor registered once in
// JNI_OnLoad.
JNIEnv* getJniEnv();

// Called from JNI_OnLoad exactly once. Caches jclass+jmethodIDs.
// Returns false if the RiseCallback class or any expected method is missing.
bool cacheCallbackIds(JNIEnv* env);

// Pack an (top, left, bottom, right) rectangle into a single jlong.
// All four values must fit in uint16 (scenes > 65535 pixels per axis are
// not supported on Android anyway given memory constraints).
inline jlong packRect(unsigned t, unsigned l, unsigned b, unsigned r) {
    return (static_cast<jlong>(t & 0xFFFF) << 48) |
           (static_cast<jlong>(l & 0xFFFF) << 32) |
           (static_cast<jlong>(b & 0xFFFF) << 16) |
           (static_cast<jlong>(r & 0xFFFF));
}

// RAII local-frame guard. Scope this at the top of every JNI callback body
// that creates local refs (jstrings, jobjects returned from Object methods,
// etc.) so tile callbacks do not leak refs across hundreds of invocations.
class ScopedLocalFrame {
public:
    ScopedLocalFrame(JNIEnv* env, jint capacity) : env_(env), ok_(false) {
        ok_ = (env_->PushLocalFrame(capacity) == JNI_OK);
    }
    ~ScopedLocalFrame() {
        if (ok_) {
            env_->PopLocalFrame(nullptr);
        }
    }
    ScopedLocalFrame(const ScopedLocalFrame&) = delete;
    ScopedLocalFrame& operator=(const ScopedLocalFrame&) = delete;
    bool ok() const { return ok_; }
private:
    JNIEnv* env_;
    bool    ok_;
};

} // namespace rise_jni

#endif // RISE_JNI_UTIL_H_
