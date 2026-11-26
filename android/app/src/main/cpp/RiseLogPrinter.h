// RiseLogPrinter.h — implements RISE::ILogPrinter backed by the Android
// __android_log_print API and a pass-through to the bridge (which forwards
// log lines to the Kotlin RiseCallback).
//
// The library's log subsystem holds printers via refcounted pointers, so
// this class inherits from RISE::Implementation::Reference for the
// addref/release machinery — the same pattern StreamPrinter.h uses.

#ifndef RISE_LOG_PRINTER_H_
#define RISE_LOG_PRINTER_H_

#include "Interfaces/ILogPrinter.h"
#include "Utilities/Reference.h"

namespace rise_jni {

class RiseBridge;

class LogPrinterImpl :
    public virtual RISE::ILogPrinter,
    public virtual RISE::Implementation::Reference
{
public:
    explicit LogPrinterImpl(RiseBridge* bridge);

    // ILogPrinter
    void Print(const RISE::LogEvent& event) override;
    void Flush() override;

protected:
    ~LogPrinterImpl() override;

private:
    RiseBridge* m_bridge;
};

} // namespace rise_jni

#endif // RISE_LOG_PRINTER_H_
