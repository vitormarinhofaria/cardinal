#include <windows.h>
#include <wincred.h>

#include <stdexcept>
#include <string>

#include "cardinal/credentials.h"

namespace cardinal {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

[[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string(what) + ": error " + std::to_string(::GetLastError()));
}

class WinCredStore : public CredentialStore {
public:
    explicit WinCredStore(std::string account) : target_(widen("Cardinal/" + account)) {}

    std::optional<Credentials> load() override {
        PCREDENTIALW cred = nullptr;
        if (!::CredReadW(target_.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
            if (::GetLastError() == ERROR_NOT_FOUND) return std::nullopt;
            fail("credential read");
        }
        Credentials c = decodeCredentials(std::string(
            reinterpret_cast<const char*>(cred->CredentialBlob), cred->CredentialBlobSize));
        ::CredFree(cred);
        return c;
    }

    void save(const Credentials& c) override {
        std::string secret = encodeCredentials(c);
        CREDENTIALW cred{};
        cred.Type = CRED_TYPE_GENERIC;
        cred.TargetName = const_cast<LPWSTR>(target_.c_str());
        cred.CredentialBlobSize = static_cast<DWORD>(secret.size());
        cred.CredentialBlob = reinterpret_cast<LPBYTE>(secret.data());
        cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
        if (!::CredWriteW(&cred, 0)) fail("credential write");
    }

    void clear() override {
        if (!::CredDeleteW(target_.c_str(), CRED_TYPE_GENERIC, 0) &&
            ::GetLastError() != ERROR_NOT_FOUND)
            fail("credential delete");
    }

private:
    std::wstring target_;
};

}  // namespace

std::unique_ptr<CredentialStore> makeWinCredCredentialStore(std::string account) {
    return std::make_unique<WinCredStore>(std::move(account));
}

}  // namespace cardinal
