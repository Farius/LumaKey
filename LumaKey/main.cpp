#include <windows.h>
#include <shellapi.h>
#include <wbemidl.h>
#include <comdef.h>
#include <wtsapi32.h>

#include "resource.h"

#include <algorithm>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wtsapi32.lib")

namespace {

constexpr wchar_t kWindowClassName[] = L"LumaKeyHiddenWindow";
constexpr wchar_t kTrayTip[] = L"LumaKey";
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr int kHotkeyBrightnessUp = 1001;
constexpr int kHotkeyBrightnessDown = 1002;
constexpr int kMenuBrightnessUp = 2001;
constexpr int kMenuBrightnessDown = 2002;
constexpr int kMenuExit = 2003;
constexpr int kDefaultStep = 10;
constexpr UINT_PTR kHotkeyWatchdogTimerId = 1;
constexpr UINT kHotkeyWatchdogIntervalMs = 30000;

HWND g_hwnd = nullptr;
NOTIFYICONDATAW g_nid{};
UINT g_taskbarCreatedMessage = 0;
int g_brightnessStep = kDefaultStep;
std::wstring g_exeDir;
std::wstring g_settingsPath;
std::wstring g_logPath;

std::wstring GetLastErrorText(DWORD error = GetLastError()) {
    if (error == 0) {
        return L"No error";
    }

    wchar_t* buffer = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    std::wstring message = size && buffer ? buffer : L"Unknown error";
    if (buffer) {
        LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

void LogLine(const std::wstring& message) {
    std::wofstream log(g_logPath, std::ios::app);
    if (!log) {
        return;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    log << L"["
        << st.wYear << L"-" << st.wMonth << L"-" << st.wDay << L" "
        << st.wHour << L":" << st.wMinute << L":" << st.wSecond << L"] "
        << message << L"\n";
}

std::wstring GetExecutableDirectory() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH) {
        return L".";
    }

    std::wstring fullPath(path, length);
    const size_t slash = fullPath.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : fullPath.substr(0, slash);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& file) {
    if (dir.empty() || dir == L".") {
        return file;
    }
    if (dir.back() == L'\\' || dir.back() == L'/') {
        return dir + file;
    }
    return dir + L"\\" + file;
}

void EnsureDefaultSettingsFile() {
    const DWORD attrs = GetFileAttributesW(g_settingsPath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return;
    }

    std::wofstream settings(g_settingsPath);
    if (settings) {
        settings << L"[Brightness]\nStep=10\n";
    }
}

int LoadBrightnessStep() {
    EnsureDefaultSettingsFile();

    const int value = GetPrivateProfileIntW(L"Brightness", L"Step", kDefaultStep, g_settingsPath.c_str());
    if (value < 1 || value > 100) {
        return kDefaultStep;
    }
    return value;
}

void ShowTrayBalloon(const std::wstring& title, const std::wstring& text, DWORD icon = NIIF_INFO) {
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = icon;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    nid.uTimeout = 3000;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void ShowError(const std::wstring& message) {
    LogLine(message);
    ShowTrayBalloon(L"LumaKey error", message, NIIF_ERROR);
}

HICON LoadLumaKeyIcon(int width, int height) {
    HICON icon = reinterpret_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_LUMAKEY_ICON),
                   IMAGE_ICON, width, height, LR_DEFAULTCOLOR | LR_SHARED));
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

std::wstring HResultText(HRESULT hr) {
    std::wstringstream stream;
    stream << L"HRESULT 0x"
           << std::hex << std::uppercase << std::setw(8) << std::setfill(L'0')
           << static_cast<unsigned long>(hr);
    return stream.str();
}

void LogHResult(const std::wstring& step, HRESULT hr) {
    LogLine(step + L": " + HResultText(hr));
}

class WmiBrightnessController {
public:
    WmiBrightnessController() = default;
    ~WmiBrightnessController() { Release(); }

    WmiBrightnessController(const WmiBrightnessController&) = delete;
    WmiBrightnessController& operator=(const WmiBrightnessController&) = delete;

    HRESULT Initialize() {
        HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IWbemLocator, reinterpret_cast<void**>(&locator_));
        LogHResult(L"CoCreateInstance IWbemLocator", hr);
        if (FAILED(hr)) {
            return hr;
        }

        BSTR ns = SysAllocString(L"ROOT\\WMI");
        hr = locator_->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services_);
        SysFreeString(ns);
        LogHResult(L"ConnectServer ROOT\\WMI", hr);
        if (FAILED(hr)) {
            return hr;
        }

        hr = CoSetProxyBlanket(services_, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                               RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                               nullptr, EOAC_NONE);
        LogHResult(L"CoSetProxyBlanket IWbemServices", hr);
        return hr;
    }

    HRESULT GetCurrentBrightness(int& brightness) {
        brightness = -1;
        IEnumWbemClassObject* enumerator = nullptr;
        BSTR wql = SysAllocString(L"WQL");
        BSTR query = SysAllocString(L"SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active = TRUE");
        HRESULT hr = services_->ExecQuery(wql, query,
                                          WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                          nullptr, &enumerator);
        SysFreeString(query);
        SysFreeString(wql);
        LogHResult(L"ExecQuery WmiMonitorBrightness active instance", hr);
        if (FAILED(hr)) {
            return hr;
        }

        IWbemClassObject* object = nullptr;
        ULONG returned = 0;
        hr = enumerator->Next(3000, 1, &object, &returned);
        enumerator->Release();
        if (FAILED(hr)) {
            return hr;
        }
        if (returned == 0 || object == nullptr) {
            BSTR fallbackWql = SysAllocString(L"WQL");
            BSTR fallbackQuery = SysAllocString(L"SELECT CurrentBrightness FROM WmiMonitorBrightness");
            hr = services_->ExecQuery(fallbackWql, fallbackQuery,
                                      WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                      nullptr, &enumerator);
            SysFreeString(fallbackQuery);
            SysFreeString(fallbackWql);
            LogHResult(L"ExecQuery WmiMonitorBrightness fallback instance", hr);
            if (FAILED(hr)) {
                return hr;
            }

            hr = enumerator->Next(3000, 1, &object, &returned);
            enumerator->Release();
            if (FAILED(hr)) {
                return hr;
            }
            if (returned == 0 || object == nullptr) {
                return WBEM_E_NOT_FOUND;
            }
        }

        VARIANT value;
        VariantInit(&value);
        hr = object->Get(L"CurrentBrightness", 0, &value, nullptr, nullptr);
        object->Release();
        LogHResult(L"Get CurrentBrightness", hr);
        if (SUCCEEDED(hr)) {
            if (value.vt == VT_UI1) {
                brightness = value.bVal;
            } else if (value.vt == VT_I4) {
                brightness = value.intVal;
            } else if (value.vt == VT_UI4) {
                brightness = static_cast<int>(value.uintVal);
            } else {
                hr = WBEM_E_TYPE_MISMATCH;
            }
        }
        VariantClear(&value);
        return hr;
    }

    HRESULT SetBrightness(int brightness) {
        brightness = std::clamp(brightness, 0, 100);

        IEnumWbemClassObject* enumerator = nullptr;
        BSTR wql = SysAllocString(L"WQL");
        BSTR query = SysAllocString(L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active = TRUE");
        HRESULT hr = services_->ExecQuery(wql, query,
                                          WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                          nullptr, &enumerator);
        SysFreeString(query);
        SysFreeString(wql);
        LogHResult(L"ExecQuery WmiMonitorBrightnessMethods active instance", hr);
        if (FAILED(hr)) {
            return hr;
        }

        IWbemClassObject* object = nullptr;
        ULONG returned = 0;
        hr = enumerator->Next(3000, 1, &object, &returned);
        enumerator->Release();
        if (FAILED(hr)) {
            return hr;
        }
        if (returned == 0 || object == nullptr) {
            BSTR fallbackWql = SysAllocString(L"WQL");
            BSTR fallbackQuery = SysAllocString(L"SELECT * FROM WmiMonitorBrightnessMethods");
            hr = services_->ExecQuery(fallbackWql, fallbackQuery,
                                      WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                      nullptr, &enumerator);
            SysFreeString(fallbackQuery);
            SysFreeString(fallbackWql);
            LogHResult(L"ExecQuery WmiMonitorBrightnessMethods fallback instance", hr);
            if (FAILED(hr)) {
                return hr;
            }

            hr = enumerator->Next(3000, 1, &object, &returned);
            enumerator->Release();
            if (FAILED(hr)) {
                return hr;
            }
            if (returned == 0 || object == nullptr) {
                return WBEM_E_NOT_FOUND;
            }
        }

        VARIANT objectPath;
        VariantInit(&objectPath);
        hr = object->Get(L"__PATH", 0, &objectPath, nullptr, nullptr);
        object->Release();
        LogHResult(L"Get WmiMonitorBrightnessMethods __PATH", hr);
        if (FAILED(hr) || objectPath.vt != VT_BSTR) {
            if (SUCCEEDED(hr)) {
                LogLine(L"Get WmiMonitorBrightnessMethods __PATH returned non-BSTR VARIANT");
            }
            VariantClear(&objectPath);
            return FAILED(hr) ? hr : WBEM_E_TYPE_MISMATCH;
        }
        LogLine(L"WmiSetBrightness object path: " + std::wstring(objectPath.bstrVal));

        IWbemClassObject* methodClass = nullptr;
        BSTR className = SysAllocString(L"WmiMonitorBrightnessMethods");
        hr = services_->GetObject(className, 0, nullptr, &methodClass, nullptr);
        SysFreeString(className);
        LogHResult(L"GetObject WmiMonitorBrightnessMethods class", hr);
        if (FAILED(hr)) {
            VariantClear(&objectPath);
            return hr;
        }

        IWbemClassObject* inSignature = nullptr;
        BSTR methodName = SysAllocString(L"WmiSetBrightness");
        hr = methodClass->GetMethod(methodName, 0, &inSignature, nullptr);
        methodClass->Release();
        LogHResult(L"GetMethod WmiSetBrightness", hr);
        if (FAILED(hr)) {
            SysFreeString(methodName);
            VariantClear(&objectPath);
            return hr;
        }

        IWbemClassObject* inParams = nullptr;
        hr = inSignature->SpawnInstance(0, &inParams);
        inSignature->Release();
        LogHResult(L"SpawnInstance WmiSetBrightness input parameters", hr);
        if (FAILED(hr)) {
            SysFreeString(methodName);
            VariantClear(&objectPath);
            return hr;
        }

        VARIANT timeout;
        VariantInit(&timeout);
        timeout.vt = VT_I4;
        timeout.lVal = 1;
        hr = inParams->Put(L"Timeout", 0, &timeout, 0);
        VariantClear(&timeout);
        LogHResult(L"Put WmiSetBrightness Timeout=1", hr);

        if (SUCCEEDED(hr)) {
            VARIANT targetBrightness;
            VariantInit(&targetBrightness);
            targetBrightness.vt = VT_UI1;
            targetBrightness.bVal = static_cast<BYTE>(brightness);
            HRESULT brightnessHr = inParams->Put(L"Brightness", 0, &targetBrightness, 0);
            VariantClear(&targetBrightness);
            LogHResult(L"Put WmiSetBrightness Brightness as VT_UI1", brightnessHr);

            if (FAILED(brightnessHr)) {
                VARIANT fallbackBrightness;
                VariantInit(&fallbackBrightness);
                fallbackBrightness.vt = VT_I4;
                fallbackBrightness.lVal = brightness;
                brightnessHr = inParams->Put(L"Brightness", 0, &fallbackBrightness, 0);
                VariantClear(&fallbackBrightness);
                LogHResult(L"Put WmiSetBrightness Brightness as VT_I4 fallback", brightnessHr);
            }
            hr = brightnessHr;
        }

        if (SUCCEEDED(hr)) {
            IWbemClassObject* outParams = nullptr;
            hr = services_->ExecMethod(objectPath.bstrVal, methodName, 0, nullptr, inParams, &outParams, nullptr);
            LogHResult(L"ExecMethod WmiSetBrightness", hr);
            if (SUCCEEDED(hr) && outParams != nullptr) {
                VARIANT returnValue;
                VariantInit(&returnValue);
                HRESULT returnHr = outParams->Get(L"ReturnValue", 0, &returnValue, nullptr, nullptr);
                LogHResult(L"Get WmiSetBrightness ReturnValue", returnHr);
                if (SUCCEEDED(returnHr)) {
                    if (returnValue.vt == VT_I4 && returnValue.intVal != 0) {
                        hr = HRESULT_FROM_WIN32(static_cast<DWORD>(returnValue.intVal));
                    } else if (returnValue.vt == VT_UI4 && returnValue.uintVal != 0) {
                        hr = HRESULT_FROM_WIN32(returnValue.uintVal);
                    }
                }
                VariantClear(&returnValue);
                outParams->Release();
            }
            if (FAILED(hr)) {
                LogHResult(L"ExecMethod WmiSetBrightness final failure", hr);
            }
        }

        inParams->Release();
        SysFreeString(methodName);
        VariantClear(&objectPath);
        return hr;
    }

private:
    void Release() {
        if (services_) {
            services_->Release();
            services_ = nullptr;
        }
        if (locator_) {
            locator_->Release();
            locator_ = nullptr;
        }
    }

    IWbemLocator* locator_ = nullptr;
    IWbemServices* services_ = nullptr;
};

void AdjustBrightness(int delta) {
    WmiBrightnessController controller;
    HRESULT hr = controller.Initialize();
    if (FAILED(hr)) {
        ShowError(L"Failed to initialize WMI brightness control: " + HResultText(hr));
        return;
    }

    int current = 0;
    hr = controller.GetCurrentBrightness(current);
    if (FAILED(hr)) {
        ShowError(L"Failed to read brightness: " + HResultText(hr));
        return;
    }
    LogLine(L"Current brightness: " + std::to_wstring(current) + L"%");

    const int next = std::clamp(current + delta, 0, 100);
    LogLine(L"Target brightness: " + std::to_wstring(next) + L"%");
    hr = controller.SetBrightness(next);
    if (FAILED(hr)) {
        ShowError(L"Failed to set brightness: " + HResultText(hr));
        return;
    }

    ShowTrayBalloon(L"LumaKey", L"Brightness: " + std::to_wstring(next) + L"%", NIIF_INFO);
}

void AddTrayIcon(HWND hwnd) {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = kTrayIconId;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = kTrayCallbackMessage;
    g_nid.hIcon = LoadLumaKeyIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    wcsncpy_s(g_nid.szTip, kTrayTip, _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, kMenuBrightnessUp, L"Brightness +");
    AppendMenuW(menu, MF_STRING, kMenuBrightnessDown, L"Brightness -");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

void RegisterBrightnessHotkeys(HWND hwnd) {
    if (!RegisterHotKey(hwnd, kHotkeyBrightnessUp, MOD_WIN | MOD_SHIFT, VK_OEM_6)) {
        ShowError(L"Failed to register Win+Shift+] hotkey: " + GetLastErrorText());
    }
    if (!RegisterHotKey(hwnd, kHotkeyBrightnessDown, MOD_WIN | MOD_SHIFT, VK_OEM_4)) {
        ShowError(L"Failed to register Win+Shift+[ hotkey: " + GetLastErrorText());
    }
}

void UnregisterBrightnessHotkeys(HWND hwnd) {
    UnregisterHotKey(hwnd, kHotkeyBrightnessUp);
    UnregisterHotKey(hwnd, kHotkeyBrightnessDown);
}

// Hotkey registrations can become stale after sleep/resume or session
// lock/unlock. Force a fresh registration when those events arrive.
void ReRegisterBrightnessHotkeys(HWND hwnd, const wchar_t* reason) {
    LogLine(std::wstring(L"Re-registering hotkeys: ") + reason);
    UnregisterBrightnessHotkeys(hwnd);
    RegisterBrightnessHotkeys(hwnd);
}

// Non-destructive periodic check: RegisterHotKey fails with
// ERROR_HOTKEY_ALREADY_REGISTERED while our registration is still alive,
// so a success here means the registration had been lost and is now back.
void WatchdogCheckHotkeys(HWND hwnd) {
    if (RegisterHotKey(hwnd, kHotkeyBrightnessUp, MOD_WIN | MOD_SHIFT, VK_OEM_6)) {
        LogLine(L"Watchdog restored Win+Shift+] hotkey");
    }
    if (RegisterHotKey(hwnd, kHotkeyBrightnessDown, MOD_WIN | MOD_SHIFT, VK_OEM_4)) {
        LogLine(L"Watchdog restored Win+Shift+[ hotkey");
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == g_taskbarCreatedMessage && g_taskbarCreatedMessage != 0) {
        // Explorer restarted: the previous tray icon is gone, add it again.
        LogLine(L"TaskbarCreated received, restoring tray icon");
        AddTrayIcon(hwnd);
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        AddTrayIcon(hwnd);
        RegisterBrightnessHotkeys(hwnd);
        WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
        SetTimer(hwnd, kHotkeyWatchdogTimerId, kHotkeyWatchdogIntervalMs, nullptr);
        return 0;
    case WM_POWERBROADCAST:
        if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) {
            ReRegisterBrightnessHotkeys(hwnd, L"resume from sleep");
        }
        return TRUE;
    case WM_WTSSESSION_CHANGE:
        if (wparam == WTS_SESSION_UNLOCK || wparam == WTS_SESSION_LOGON) {
            ReRegisterBrightnessHotkeys(hwnd, L"session unlock");
        }
        return 0;
    case WM_TIMER:
        if (wparam == kHotkeyWatchdogTimerId) {
            WatchdogCheckHotkeys(hwnd);
            return 0;
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kMenuBrightnessUp:
            AdjustBrightness(g_brightnessStep);
            return 0;
        case kMenuBrightnessDown:
            AdjustBrightness(-g_brightnessStep);
            return 0;
        case kMenuExit:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_HOTKEY:
        if (wparam == kHotkeyBrightnessUp) {
            AdjustBrightness(g_brightnessStep);
            return 0;
        }
        if (wparam == kHotkeyBrightnessDown) {
            AdjustBrightness(-g_brightnessStep);
            return 0;
        }
        break;
    case kTrayCallbackMessage:
        if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_CONTEXTMENU) {
            ShowTrayMenu(hwnd);
            return 0;
        }
        break;
    case WM_DESTROY:
        KillTimer(hwnd, kHotkeyWatchdogTimerId);
        WTSUnRegisterSessionNotification(hwnd);
        UnregisterBrightnessHotkeys(hwnd);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_exeDir = GetExecutableDirectory();
    g_settingsPath = JoinPath(g_exeDir, L"settings.ini");
    g_logPath = JoinPath(g_exeDir, L"LumaKey.log");
    g_brightnessStep = LoadBrightnessStep();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        LogLine(L"Failed to initialize COM: " + HResultText(hr));
        return 1;
    }

    hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                              RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                              nullptr, EOAC_NONE, nullptr);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE) {
        LogLine(L"Failed to initialize COM security: " + HResultText(hr));
        CoUninitialize();
        return 1;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClassName;
    wc.hIcon = LoadLumaKeyIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));

    if (!RegisterClassW(&wc)) {
        LogLine(L"Failed to register window class: " + GetLastErrorText());
        CoUninitialize();
        return 1;
    }

    // Broadcast messages (WM_POWERBROADCAST, TaskbarCreated) never reach
    // message-only windows, so use an invisible top-level window instead.
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClassName, L"LumaKey",
                             WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                             nullptr, instance, nullptr);
    if (!g_hwnd) {
        LogLine(L"Failed to create hidden window: " + GetLastErrorText());
        CoUninitialize();
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
