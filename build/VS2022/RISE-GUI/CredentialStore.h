//////////////////////////////////////////////////////////////////////
//
//  CredentialStore.h - OS-backed per-provider API-key storage for the
//    Windows Qt6 ChatPanel (Windows Credential Manager via <wincred.h>).
//
//    Mirrors macOS AgentChatKeychain (ChatViewModel.swift):
//      target: "RISE Agent Chat/<account>"  (CRED_TYPE_GENERIC)
//      persist: CRED_PERSIST_LOCAL_MACHINE  (current user profile on this PC)
//
//    SECRET HYGIENE:
//      * Never stores keys in QSettings / registry / plaintext files.
//      * Never logs secrets.
//      * Scrubs temporary UTF-8 byte buffers via SecureZeroMemory
//        immediately after CredWriteW / CredReadW.
//
//////////////////////////////////////////////////////////////////////

#ifndef CREDENTIALSTORE_H
#define CREDENTIALSTORE_H

#include <QString>

struct CredentialStore
{
    static constexpr const char* kServiceName = "RISE Agent Chat";

    /// Read the stored secret for `account` ("openai", "anthropic",
    /// "gemini", "xai"), or return an empty QString when absent/unreadable.
    static QString read(const QString& account);

    /// Create or update the stored secret for `account`.  An empty/whitespace
    /// secret delegates to remove().  Returns false on an OS credential error.
    static bool write(const QString& account, const QString& secret);

    /// Delete the stored secret for `account`.  Absent-item deletes return true.
    static bool remove(const QString& account);

    /// Presence check without constructing a QString copy of the secret.
    static bool exists(const QString& account);
};

#endif // CREDENTIALSTORE_H
