#include "SecureStorage.hpp"

#if defined(Q_OS_WIN)
#include <windows.h>
#include <wincred.h>
#elif defined(Q_OS_MACOS)
#include <Security/Security.h>
#endif

namespace account::securestorage {

#if defined(Q_OS_WIN)
namespace {
constexpr wchar_t kCredentialTarget[] = L"VLTStudioPro/DesktopAccount";
}

bool write(const QByteArray& value) {
    if (value.isEmpty() || value.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) return false;
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(kCredentialTarget);
    credential.CredentialBlobSize = DWORD(value.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(value.constData()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t*>(L"VLT Studio Pro");
    return CredWriteW(&credential, 0) != FALSE;
}

QByteArray read() {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(kCredentialTarget, CRED_TYPE_GENERIC, 0, &credential)) return {};
    const QByteArray value(reinterpret_cast<const char*>(credential->CredentialBlob),
                           int(credential->CredentialBlobSize));
    CredFree(credential);
    return value;
}

bool clear() {
    return CredDeleteW(kCredentialTarget, CRED_TYPE_GENERIC, 0) != FALSE ||
           GetLastError() == ERROR_NOT_FOUND;
}

#elif defined(Q_OS_MACOS)
namespace {
CFStringRef service() { return CFSTR("com.vltstudio.pro.desktop-account"); }
CFStringRef account() { return CFSTR("desktop-session"); }

CFMutableDictionaryRef baseQuery() {
    auto query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, service());
    CFDictionarySetValue(query, kSecAttrAccount, account());
    return query;
}
}

bool write(const QByteArray& value) {
    if (value.isEmpty()) return false;
    CFDataRef data = CFDataCreate(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(value.constData()), value.size());
    CFMutableDictionaryRef query = baseQuery();
    CFMutableDictionaryRef update = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(update, kSecValueData, data);
    OSStatus status = SecItemUpdate(query, update);
    if (status == errSecItemNotFound) {
        CFDictionarySetValue(query, kSecValueData, data);
        status = SecItemAdd(query, nullptr);
    }
    CFRelease(update);
    CFRelease(query);
    CFRelease(data);
    return status == errSecSuccess;
}

QByteArray read() {
    CFMutableDictionaryRef query = baseQuery();
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (status != errSecSuccess || !result) return {};
    CFDataRef data = static_cast<CFDataRef>(result);
    QByteArray value(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                     int(CFDataGetLength(data)));
    CFRelease(result);
    return value;
}

bool clear() {
    CFMutableDictionaryRef query = baseQuery();
    const OSStatus status = SecItemDelete(query);
    CFRelease(query);
    return status == errSecSuccess || status == errSecItemNotFound;
}

#else
bool write(const QByteArray&) { return false; }
QByteArray read() { return {}; }
bool clear() { return true; }
#endif

} // namespace account::securestorage
