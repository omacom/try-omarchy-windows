#include <windows.h>
#include <userconsentverifierinterop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Credentials.h>
#include <winrt/Windows.Security.Credentials.UI.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Security.Cryptography.Core.h>
#include <winrt/Windows.UI.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace wf = winrt::Windows::Foundation;
namespace wc = winrt::Windows::Security::Credentials;
namespace wcu = winrt::Windows::Security::Credentials::UI;
namespace wcrypto = winrt::Windows::Security::Cryptography;
namespace wcore = winrt::Windows::Security::Cryptography::Core;

namespace {

constexpr wchar_t class_name[] = L"TryOmarchyHelloOwner";
constexpr UINT worker_done = WM_APP + 1;

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == worker_done || message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool valid_guest_id(std::wstring const& value) {
    if (value.size() != 64) return false;
    for (wchar_t character : value) {
        if (!((character >= L'0' && character <= L'9') ||
              (character >= L'a' && character <= L'f'))) return false;
    }
    return true;
}

std::vector<uint8_t> read_payload() {
    std::vector<uint8_t> result;
    char byte;
    while (std::cin.get(byte)) {
        if (result.size() >= 4096) throw std::runtime_error("payload too large");
        result.push_back(static_cast<uint8_t>(byte));
    }
    if (result.empty()) throw std::runtime_error("empty payload");
    return result;
}

std::string sign_with_key(wc::KeyCredential const& key,
                          winrt::Windows::UI::WindowId window_id,
                          std::vector<uint8_t> const& payload) {
    auto buffer = wcrypto::CryptographicBuffer::CreateFromByteArray(
        winrt::array_view<uint8_t const>(payload));
    auto result = key.RequestSignForWindowAsync(window_id, buffer).get();
    if (result.Status() != wc::KeyCredentialStatus::Success) {
        throw std::runtime_error("signing denied or unavailable");
    }
    auto public_key = key.RetrievePublicKey(
        wcore::CryptographicPublicKeyBlobType::X509SubjectPublicKeyInfo);
    return "OK " + winrt::to_string(wcrypto::CryptographicBuffer::EncodeToBase64String(public_key)) +
           " " + winrt::to_string(wcrypto::CryptographicBuffer::EncodeToBase64String(result.Result()));
}

bool request_consent(HWND window) {
    auto message = winrt::hstring(L"Approve sudo in the focused Try Omarchy guest");
    auto interop = winrt::get_activation_factory<wcu::UserConsentVerifier,
                                                 IUserConsentVerifierInterop>();
    auto operation = winrt::capture<wf::IAsyncOperation<wcu::UserConsentVerificationResult>>(
        interop, &IUserConsentVerifierInterop::RequestVerificationForWindowAsync,
        window, reinterpret_cast<HSTRING>(winrt::get_abi(message)));
    return operation.get() == wcu::UserConsentVerificationResult::Verified;
}

std::string run(std::wstring const& operation,
                std::wstring const& guest_id,
                HWND window,
                std::vector<uint8_t> const& payload) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    auto name = winrt::hstring(L"TryOmarchyHello-" + guest_id);
    auto window_id = winrt::Windows::UI::WindowId{
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(window))};
    if (operation == L"delete") {
        auto existing = wc::KeyCredentialManager::OpenAsync(name).get();
        if (existing.Status() == wc::KeyCredentialStatus::NotFound) return "OK";
        if (existing.Status() != wc::KeyCredentialStatus::Success) return "DENIED key-unavailable";
        wc::KeyCredentialManager::DeleteAsync(name).get();
        return "OK";
    }
    wc::KeyCredential key{nullptr};
    bool created = false;
    if (operation == L"enroll") {
        auto result = wc::KeyCredentialManager::RequestCreateForWindowAsync(
            window_id, name, wc::KeyCredentialCreationOption::ReplaceExisting).get();
        if (result.Status() != wc::KeyCredentialStatus::Success) return "DENIED create";
        key = result.Credential();
        created = true;
    } else {
        auto result = wc::KeyCredentialManager::OpenAsync(name).get();
        if (result.Status() != wc::KeyCredentialStatus::Success) return "DENIED key-unavailable";
        key = result.Credential();
        if (!request_consent(window)) return "DENIED consent";
    }
    try {
        return sign_with_key(key, window_id, payload);
    } catch (...) {
        if (created) {
            try { wc::KeyCredentialManager::DeleteAsync(name).get(); } catch (...) {}
        }
        throw;
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    std::wstring operation(argv[1]);
    std::wstring guest_id(argv[2]);
    if (!valid_guest_id(guest_id) ||
        (operation != L"enroll" && operation != L"sign" && operation != L"delete")) return 2;
    std::vector<uint8_t> payload;
    try {
        if (operation != L"delete") payload = read_payload();
    } catch (...) {
        return 2;
    }

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = class_name;
    if (!RegisterClassW(&window_class)) return 3;
    HWND window = CreateWindowExW(
        WS_EX_APPWINDOW, class_name, L"Try Omarchy Windows Hello",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 450, 160,
        nullptr, nullptr, window_class.hInstance, nullptr);
    if (!window) return 3;
    CreateWindowExW(0, L"STATIC",
        operation == L"sign" ? L"Approve sudo in the focused Omarchy guest." :
        L"Pair Windows Hello with this Omarchy guest.",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        20, 35, 400, 70, window, nullptr, window_class.hInstance, nullptr);
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
    SetForegroundWindow(window);

    std::string answer = "DENIED unavailable";
    std::thread worker([&] {
        try {
            answer = run(operation, guest_id, window, payload);
        } catch (winrt::hresult_error const&) {
            answer = "DENIED windows-error";
        } catch (std::exception const&) {
            answer = "DENIED operation-error";
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
    std::cout << answer << '\n';
    return answer.starts_with("OK") ? 0 : 1;
}
