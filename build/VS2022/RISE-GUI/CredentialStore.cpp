//////////////////////////////////////////////////////////////////////
//
//  CredentialStore.cpp - Windows Credential Manager (<wincred.h>) wrapper
//    for ChatPanel per-provider API keys.
//
//////////////////////////////////////////////////////////////////////

#include "CredentialStore.h"

#include <QByteArray>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#include <cstring>
#include <string>

namespace {

std::wstring targetNameForAccount(const QString& account)
{
    const QString full = QString::fromLatin1(CredentialStore::kServiceName)
        + QLatin1Char('/') + account.trimmed();
    return full.toStdWString();
}

}  // namespace

QString CredentialStore::read(const QString& account)
{
    if (account.trimmed().isEmpty()) return QString();
    const std::wstring target = targetNameForAccount(account);

    PCREDENTIALW pcred = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &pcred) || !pcred) {
        return QString();
    }

    QString secret;
    if (pcred->CredentialBlob && pcred->CredentialBlobSize > 0) {
        const DWORD sz = pcred->CredentialBlobSize;
        // Standard Win32 `CRED_TYPE_GENERIC` blobs (Credential Manager UI,
        // `cmdkey.exe`, and `CredentialStore::write` below) are UTF-16LE
        // `wchar_t` code units without a trailing NUL.
        if ((sz % sizeof(char16_t)) == 0) {
            int u16Count = static_cast<int>(sz / sizeof(char16_t));
            const char16_t* u16 = reinterpret_cast<const char16_t*>(pcred->CredentialBlob);
            while (u16Count > 0 && u16[u16Count - 1] == u'\0') {
                --u16Count;
            }
            secret = QString::fromUtf16(u16, u16Count);
        } else {
            secret = QString::fromUtf8(
                reinterpret_cast<const char*>(pcred->CredentialBlob),
                static_cast<int>(sz));
        }
        SecureZeroMemory(pcred->CredentialBlob, pcred->CredentialBlobSize);
    }
    CredFree(pcred);

    if (secret.trimmed().isEmpty()) {
        if (!secret.isEmpty()) {
            if (secret.isDetached()) {
                secret.fill(QChar(0));
            }
            secret.clear();
        }
        return QString();
    }
    return secret;
}

bool CredentialStore::write(const QString& account, const QString& secret)
{
    if (account.trimmed().isEmpty()) return false;
    const QString trimmed = secret.trimmed();
    if (trimmed.isEmpty()) {
        return remove(account);
    }

    const std::wstring target = targetNameForAccount(account);
    const qsizetype byteCount = trimmed.size() * static_cast<qsizetype>(sizeof(char16_t));
    if (byteCount <= 0 || static_cast<size_t>(byteCount) > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        return false;
    }
    QByteArray utf16Bytes(byteCount, Qt::Uninitialized);
    std::memcpy(utf16Bytes.data(), trimmed.utf16(), static_cast<size_t>(byteCount));

    CREDENTIALW cred = {};
    cred.Flags = 0;
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(target.c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(byteCount);
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(utf16Bytes.data());
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;

    const BOOL ok = CredWriteW(&cred, 0);
    SecureZeroMemory(utf16Bytes.data(), static_cast<SIZE_T>(utf16Bytes.size()));
    return ok != FALSE;
}

bool CredentialStore::remove(const QString& account)
{
    if (account.trimmed().isEmpty()) return true;
    const std::wstring target = targetNameForAccount(account);
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
        return true;
    }
    return GetLastError() == ERROR_NOT_FOUND;
}

bool CredentialStore::exists(const QString& account)
{
    if (account.trimmed().isEmpty()) return false;
    const std::wstring target = targetNameForAccount(account);

    PCREDENTIALW pcred = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &pcred) || !pcred) {
        return false;
    }

    const bool nonEmpty = (pcred->CredentialBlob != nullptr && pcred->CredentialBlobSize > 0);
    if (pcred->CredentialBlob && pcred->CredentialBlobSize > 0) {
        SecureZeroMemory(pcred->CredentialBlob, pcred->CredentialBlobSize);
    }
    CredFree(pcred);
    return nonEmpty;
}

#else

QString CredentialStore::read(const QString&) { return QString(); }
bool CredentialStore::write(const QString&, const QString&) { return false; }
bool CredentialStore::remove(const QString&) { return true; }
bool CredentialStore::exists(const QString&) { return false; }

#endif
