// Laptop preflight for #165: can one Windows Hello prompt both verify the
// user and sign a guest sudo request? The KeyCredential helper needs a
// consent prompt and a signing prompt per request. This tries the native
// WebAuthn platform authenticator instead: create one credential, sign two
// fresh challenges with user verification required, then delete the
// credential. Everything needed to verify the signatures offline is written
// to webauthn-preflight.json; verify-webauthn-preflight.py checks it.
//
// It never reads a PIN or biometric data, and it deletes its credential
// before exiting whenever Windows allows that.

#include <windows.h>
#include <bcrypt.h>
#include <webauthn.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t class_name[] = L"TryOmarchyWebAuthnPreflight";
constexpr wchar_t rp_id[] = L"preflight.try-omarchy.invalid";
constexpr UINT worker_done = WM_APP + 1;

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == worker_done || message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

std::string base64(BYTE const* data, DWORD size) {
    static char const alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (DWORD i = 0; i < size; i += 3) {
        uint32_t chunk = data[i] << 16;
        if (i + 1 < size) chunk |= data[i + 1] << 8;
        if (i + 2 < size) chunk |= data[i + 2];
        out += alphabet[(chunk >> 18) & 63];
        out += alphabet[(chunk >> 12) & 63];
        out += i + 1 < size ? alphabet[(chunk >> 6) & 63] : '=';
        out += i + 2 < size ? alphabet[chunk & 63] : '=';
    }
    return out;
}

std::string base64(std::vector<BYTE> const& data) {
    return base64(data.data(), static_cast<DWORD>(data.size()));
}

std::vector<BYTE> random_bytes(size_t size) {
    std::vector<BYTE> out(size);
    if (BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        throw std::runtime_error("random");
    }
    return out;
}

// The client data stands in for the canonical sudo request the guest will
// build. Windows hashes it into the signed data unchanged.
std::string client_data(char const* type, std::vector<BYTE> const& challenge) {
    std::string b64 = base64(challenge);
    for (char& c : b64) {
        if (c == '+') c = '-';
        if (c == '/') c = '_';
    }
    while (!b64.empty() && b64.back() == '=') b64.pop_back();
    return std::string("{\"type\":\"") + type + "\",\"challenge\":\"" + b64 +
           "\",\"origin\":\"try-omarchy:guest-sudo-preflight\"}";
}

std::string hresult(HRESULT hr) {
    std::ostringstream out;
    out << "0x" << std::hex << static_cast<uint32_t>(hr);
    PCWSTR name = WebAuthNGetErrorName(hr);
    if (name) {
        int size = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
        std::string text(size > 0 ? size - 1 : 0, '\0');
        if (size > 1) WideCharToMultiByte(CP_UTF8, 0, name, -1, text.data(), size, nullptr, nullptr);
        out << " " << text;
    }
    return out.str();
}

long long elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
}

std::string run(HWND window) {
    std::ostringstream json;
    json << "{\n  \"apiVersion\": " << WebAuthNGetApiVersionNumber() << ",\n";
    json << "  \"rpId\": \"preflight.try-omarchy.invalid\",\n";
    BOOL available = FALSE;
    HRESULT hr = WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable(&available);
    json << "  \"platformAuthenticator\": " << (SUCCEEDED(hr) && available ? "true" : "false") << ",\n";
    if (FAILED(hr) || !available) {
        json << "  \"result\": \"no-platform-authenticator\"\n}\n";
        return json.str();
    }

    WEBAUTHN_RP_ENTITY_INFORMATION rp{};
    rp.dwVersion = WEBAUTHN_RP_ENTITY_INFORMATION_CURRENT_VERSION;
    rp.pwszId = rp_id;
    rp.pwszName = L"Try Omarchy preflight";

    std::vector<BYTE> user_id = random_bytes(32);
    WEBAUTHN_USER_ENTITY_INFORMATION user{};
    user.dwVersion = WEBAUTHN_USER_ENTITY_INFORMATION_CURRENT_VERSION;
    user.cbId = static_cast<DWORD>(user_id.size());
    user.pbId = user_id.data();
    user.pwszName = L"try-omarchy-preflight";
    user.pwszDisplayName = L"Try Omarchy sudo preflight";

    WEBAUTHN_COSE_CREDENTIAL_PARAMETER algorithm{};
    algorithm.dwVersion = WEBAUTHN_COSE_CREDENTIAL_PARAMETER_CURRENT_VERSION;
    algorithm.pwszCredentialType = WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY;
    algorithm.lAlg = WEBAUTHN_COSE_ALGORITHM_ECDSA_P256_WITH_SHA256;
    WEBAUTHN_COSE_CREDENTIAL_PARAMETERS algorithms{1, &algorithm};

    std::string create_data = client_data("webauthn.create", random_bytes(32));
    WEBAUTHN_CLIENT_DATA create_client{};
    create_client.dwVersion = WEBAUTHN_CLIENT_DATA_CURRENT_VERSION;
    create_client.cbClientDataJSON = static_cast<DWORD>(create_data.size());
    create_client.pbClientDataJSON = reinterpret_cast<PBYTE>(create_data.data());
    create_client.pwszHashAlgId = WEBAUTHN_HASH_ALGORITHM_SHA_256;

    WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS create_options{};
    create_options.dwVersion = WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS_VERSION_1;
    create_options.dwTimeoutMilliseconds = 60000;
    create_options.dwAuthenticatorAttachment = WEBAUTHN_AUTHENTICATOR_ATTACHMENT_PLATFORM;
    create_options.dwUserVerificationRequirement = WEBAUTHN_USER_VERIFICATION_REQUIREMENT_REQUIRED;
    create_options.dwAttestationConveyancePreference = WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_NONE;

    auto started = std::chrono::steady_clock::now();
    PWEBAUTHN_CREDENTIAL_ATTESTATION attestation = nullptr;
    hr = WebAuthNAuthenticatorMakeCredential(window, &rp, &user, &algorithms, &create_client,
                                             &create_options, &attestation);
    json << "  \"create\": {\"hresult\": \"" << hresult(hr) << "\", \"ms\": " << elapsed_ms(started);
    if (FAILED(hr) || !attestation) {
        json << "},\n  \"result\": \"create-failed\"\n}\n";
        return json.str();
    }
    std::vector<BYTE> credential_id(attestation->pbCredentialId,
                                    attestation->pbCredentialId + attestation->cbCredentialId);
    json << ", \"credentialId\": \"" << base64(credential_id) << "\""
         << ", \"attestationObject\": \"" << base64(attestation->pbAttestationObject, attestation->cbAttestationObject) << "\""
         << ", \"clientData\": \"" << base64(reinterpret_cast<BYTE const*>(create_data.data()), static_cast<DWORD>(create_data.size())) << "\"},\n";
    WebAuthNFreeCredentialAttestation(attestation);

    WEBAUTHN_CREDENTIAL allowed{};
    allowed.dwVersion = WEBAUTHN_CREDENTIAL_CURRENT_VERSION;
    allowed.cbId = static_cast<DWORD>(credential_id.size());
    allowed.pbId = credential_id.data();
    allowed.pwszCredentialType = WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY;

    json << "  \"assertions\": [";
    bool all_signed = true;
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string data = client_data("webauthn.get", random_bytes(32));
        WEBAUTHN_CLIENT_DATA client{};
        client.dwVersion = WEBAUTHN_CLIENT_DATA_CURRENT_VERSION;
        client.cbClientDataJSON = static_cast<DWORD>(data.size());
        client.pbClientDataJSON = reinterpret_cast<PBYTE>(data.data());
        client.pwszHashAlgId = WEBAUTHN_HASH_ALGORITHM_SHA_256;

        WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS options{};
        options.dwVersion = WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS_VERSION_1;
        options.dwTimeoutMilliseconds = 60000;
        options.CredentialList = {1, &allowed};
        options.dwAuthenticatorAttachment = WEBAUTHN_AUTHENTICATOR_ATTACHMENT_PLATFORM;
        options.dwUserVerificationRequirement = WEBAUTHN_USER_VERIFICATION_REQUIREMENT_REQUIRED;

        started = std::chrono::steady_clock::now();
        PWEBAUTHN_ASSERTION assertion = nullptr;
        hr = WebAuthNAuthenticatorGetAssertion(window, rp_id, &client, &options, &assertion);
        json << (attempt ? ",\n    " : "\n    ") << "{\"hresult\": \"" << hresult(hr)
             << "\", \"ms\": " << elapsed_ms(started);
        if (SUCCEEDED(hr) && assertion) {
            json << ", \"authenticatorData\": \"" << base64(assertion->pbAuthenticatorData, assertion->cbAuthenticatorData) << "\""
                 << ", \"signature\": \"" << base64(assertion->pbSignature, assertion->cbSignature) << "\""
                 << ", \"clientData\": \"" << base64(reinterpret_cast<BYTE const*>(data.data()), static_cast<DWORD>(data.size())) << "\"";
            WebAuthNFreeAssertion(assertion);
        } else {
            all_signed = false;
        }
        json << "}";
    }
    json << "\n  ],\n";

    // Leave nothing behind on the laptop. Deletion needs API version 4.
    HMODULE module = GetModuleHandleW(L"webauthn.dll");
    using delete_fn = HRESULT(WINAPI*)(DWORD, PBYTE);
    auto remove = module ? reinterpret_cast<delete_fn>(
        GetProcAddress(module, "WebAuthNDeletePlatformCredential")) : nullptr;
    hr = remove ? remove(static_cast<DWORD>(credential_id.size()), credential_id.data()) : E_NOTIMPL;
    json << "  \"delete\": \"" << hresult(hr) << "\",\n";
    json << "  \"result\": \"" << (all_signed ? "signed" : "assertion-failed") << "\"\n}\n";
    return json.str();
}

} // namespace

int wmain() {
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = class_name;
    if (!RegisterClassW(&window_class)) return 3;
    HWND window = CreateWindowExW(
        WS_EX_APPWINDOW, class_name, L"Try Omarchy Windows Hello preflight",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 520, 180,
        nullptr, nullptr, window_class.hInstance, nullptr);
    if (!window) return 3;
    CreateWindowExW(0, L"STATIC",
        L"Windows will ask for Windows Hello three times: once to create a test "
        L"key, then once for each of two test approvals. Count the prompts.",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        20, 30, 470, 80, window, nullptr, window_class.hInstance, nullptr);
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
    SetForegroundWindow(window);

    std::string report = "{\"result\": \"exception\"}\n";
    std::thread worker([&] {
        try {
            report = run(window);
        } catch (...) {
        }
        PostMessageW(window, worker_done, 0, 0);
    });
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    worker.join();
    DestroyWindow(window);
    std::ofstream("webauthn-preflight.json", std::ios::binary) << report;
    return report.find("\"result\": \"signed\"") != std::string::npos ? 0 : 1;
}
