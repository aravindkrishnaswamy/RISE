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
        secret = QString::fromUtf8(
            reinterpret_cast<const char*>(pcred->CredentialBlob),
            static_cast<int>(pcred->CredentialBlobSize));
        SecureZeroMemory(pcred->CredentialBlob, pcred->CredentialBlobSize);
    }
    CredFree(pcred);

    return secret.trimmed().isEmpty() ? QString() : secret;
}

bool CredentialStore::write(const QString& account, const QString& secret)
{
    if (account.trimmed().isEmpty()) return false;
    const QString trimmed = secret.trimmed();
    if (trimmed.isEmpty()) {
        return remove(account);
    }

    const std::wstring target = targetNameForAccount(account);
    QByteArray utf8 = trimmed.toUtf8();
    if (utf8.isEmpty() || static_cast<size_t>(utf8.size()) > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        if (!utf8.isEmpty()) {
            SecureZeroMemory(utf8.data(), static_cast<SIZE_T>(utf8.size()));
        }
        return false;
    }

    CREDENTIALW cred = {};
    cred.Flags = 0;
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(target.c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(utf8.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(utf8.data());
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;

    const BOOL ok = CredWriteW(&cred, 0);
    SecureZeroMemory(utf8.data(), static_cast<SIZE_T>(utf8.size()));
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
