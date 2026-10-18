#include "ProtectedCredentialStore.h"

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#include <vector>
#endif

namespace DjehutiCredentials
{
#if defined(_WIN32)
namespace
{
std::string winError(const char* operation)
{
    return std::string(operation) + " failed (Windows error " + std::to_string(GetLastError()) + ").";
}
}

bool write(const std::string& name, const std::string& secret, std::string& error)
{
    DATA_BLOB input { static_cast<DWORD>(secret.size()),
                      reinterpret_cast<BYTE*>(const_cast<char*>(secret.data())) };
    DATA_BLOB encrypted {};
    DATA_BLOB entropy { static_cast<DWORD>(name.size()),
                        reinterpret_cast<BYTE*>(const_cast<char*>(name.data())) };
    if (!CryptProtectData(&input, L"DjehutiMath credential", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &encrypted))
    {
        error = winError("CryptProtectData");
        return false;
    }

    HKEY key = nullptr;
    const auto keyPath = std::wstring(L"Software\\DjehutiMath\\Credentials\\")
                       + std::wstring(name.begin(), name.end());
    const auto result = RegCreateKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, nullptr, 0,
                                        KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS)
    {
        LocalFree(encrypted.pbData);
        error = "Could not open protected credential storage.";
        return false;
    }
    const auto valueResult = RegSetValueExW(key, L"payload", 0, REG_BINARY,
                                            encrypted.pbData, encrypted.cbData);
    RegCloseKey(key);
    LocalFree(encrypted.pbData);
    if (valueResult != ERROR_SUCCESS)
    {
        error = "Could not write protected credential storage.";
        return false;
    }
    return true;
}

bool read(const std::string& name, std::string& secret, std::string& error)
{
    HKEY key = nullptr;
    const auto keyPath = std::wstring(L"Software\\DjehutiMath\\Credentials\\")
                       + std::wstring(name.begin(), name.end());
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
    {
        error = "No protected credential is stored.";
        return false;
    }
    DWORD type = 0, size = 0;
    auto result = RegQueryValueExW(key, L"payload", nullptr, &type, nullptr, &size);
    if (result != ERROR_SUCCESS || type != REG_BINARY || size == 0)
    {
        RegCloseKey(key);
        error = "Protected credential storage is invalid.";
        return false;
    }
    std::vector<BYTE> bytes(size);
    result = RegQueryValueExW(key, L"payload", nullptr, &type, bytes.data(), &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS)
    {
        error = "Could not read protected credential storage.";
        return false;
    }
    DATA_BLOB encrypted { size, bytes.data() };
    DATA_BLOB entropy { static_cast<DWORD>(name.size()),
                        reinterpret_cast<BYTE*>(const_cast<char*>(name.data())) };
    DATA_BLOB decrypted {};
    if (!CryptUnprotectData(&encrypted, nullptr, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &decrypted))
    {
        error = winError("CryptUnprotectData");
        return false;
    }
    secret.assign(reinterpret_cast<char*>(decrypted.pbData), decrypted.cbData);
    LocalFree(decrypted.pbData);
    return true;
}

bool remove(const std::string& name, std::string& error)
{
    const auto keyPath = std::wstring(L"Software\\DjehutiMath\\Credentials\\")
                       + std::wstring(name.begin(), name.end());
    const auto result = RegDeleteTreeW(HKEY_CURRENT_USER, keyPath.c_str());
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND)
    {
        error = "Could not remove protected credential storage.";
        return false;
    }
    return true;
}
#else
bool write(const std::string&, const std::string&, std::string& error) { error = "Protected credentials require Windows DPAPI."; return false; }
bool read(const std::string&, std::string&, std::string& error) { error = "Protected credentials require Windows DPAPI."; return false; }
bool remove(const std::string&, std::string& error) { error = "Protected credentials require Windows DPAPI."; return false; }
#endif
}
