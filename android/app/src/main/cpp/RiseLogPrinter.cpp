#include "RiseLogPrinter.h"
#include "RiseBridge.h"

#include <android/log.h>

#define LOG_TAG "RISE"

namespace rise_jni {

LogPrinterImpl::LogPrinterImpl(RiseBridge* bridge) : m_bridge(bridge) {}

LogPrinterImpl::~LogPrinterImpl() = default;

void LogPrinterImpl::Print(const RISE::LogEvent& event) {
    // Translate RISE's LOG_ENUM into Android Logcat priorities. Error and
    // Fatal are surfaced as ANDROID_LOG_ERROR so tombstone grep finds them.
    int androidPrio = ANDROID_LOG_INFO;
    switch (event.eType) {
        case RISE::eLog_Warning: androidPrio = ANDROID_LOG_WARN;  break;
        case RISE::eLog_Error:
        case RISE::eLog_Fatal:   androidPrio = ANDROID_LOG_ERROR; break;
        case RISE::eLog_Event:
        case RISE::eLog_Info:
        default:                 androidPrio = ANDROID_LOG_INFO;  break;
    }
    __android_log_print(androidPrio, LOG_TAG, "%s", event.szMessage);

    // Also forward to the Kotlin side so an in-app log pane can display it.
    // Passing the same numeric value as Logcat lets the UI colour-code.
    if (m_bridge) {
        m_bridge->onLogLine(androidPrio, event.szMessage);
    }
}

void LogPrinterImpl::Flush() {
    // __android_log_print writes directly; no buffered state to flush.
}

} // namespace rise_jni
