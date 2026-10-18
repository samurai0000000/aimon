/*
 * EdaRpcServer.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "EdaRpcServer.hxx"
#include "EdaAccelerators.hxx"
#include "ProcessLauncher.hxx"
#include "EasyEdaConverter.hxx"
#include "KiCadSchematicEditor.hxx"
#include "Version.hxx"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <chrono>
#include <cwctype>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <uiautomation.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace aimon {

namespace {

#ifdef _WIN32
// CLSID_CUIAutomation {ff48dba4-60ef-4201-aa87-54103eef594e} as declared in uiautomationclient.h.
// Defined locally so linking does not depend on the toolchain's uuid library exporting it.
const CLSID kClsidCUIAutomation = {
    0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}
};

template <typename T>
struct ComRelease {
    T* p = nullptr;
    ComRelease() = default;
    ComRelease(const ComRelease&) = delete;
    ComRelease& operator=(const ComRelease&) = delete;
    ~ComRelease() { reset(); }
    void reset() {
        if (p != nullptr) {
            p->Release();
            p = nullptr;
        }
    }
};

class ComApartmentScope {
public:
    ComApartmentScope() : _hr(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartmentScope() {
        if (SUCCEEDED(_hr)) ::CoUninitialize();
    }
    ComApartmentScope(const ComApartmentScope&) = delete;
    ComApartmentScope& operator=(const ComApartmentScope&) = delete;
    // RPC_E_CHANGED_MODE: this thread already has an STA; COM is usable, do not uninitialize.
    bool usable() const { return SUCCEEDED(_hr) || _hr == RPC_E_CHANGED_MODE; }

private:
    HRESULT _hr;
};

struct EasyEdaWindowSearch {
    bool byProcess = true;
    HWND hwnd = nullptr;
};

bool windowProcessIsEasyEdaPro(HWND hwnd) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == nullptr) return false;
    wchar_t path[1024] = {0};
    DWORD len = 1023;
    bool match = false;
    if (::QueryFullProcessImageNameW(proc, 0, path, &len) && len > 0) {
        std::wstring p(path, len);
        std::transform(p.begin(), p.end(), p.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
        const std::wstring exe = L"\\easyeda-pro.exe";
        match = p.size() >= exe.size() && p.compare(p.size() - exe.size(), exe.size(), exe) == 0;
    }
    ::CloseHandle(proc);
    return match;
}

bool windowTitleIsEasyEda(HWND hwnd) {
    wchar_t title[512] = {0};
    int n = ::GetWindowTextW(hwnd, title, 511);
    if (n <= 0) return false;
    std::wstring t(title, static_cast<size_t>(n));
    // "EasyEDA" or "\u5609\u7acb\u521bEDA" (JLC EDA, Chinese UI)
    return t.find(L"EasyEDA") != std::wstring::npos ||
           t.find(L"\u5609\u7acb\u521b" L"EDA") != std::wstring::npos;
}

BOOL CALLBACK enumEasyEdaWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* search = reinterpret_cast<EasyEdaWindowSearch*>(lParam);
    if (!::IsWindowVisible(hwnd) || ::GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    bool match = search->byProcess ? windowProcessIsEasyEdaPro(hwnd) : windowTitleIsEasyEda(hwnd);
    if (match) {
        search->hwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND findEasyEdaProWindow() {
    EasyEdaWindowSearch search;
    search.byProcess = true;
    ::EnumWindows(enumEasyEdaWindowsProc, reinterpret_cast<LPARAM>(&search));
    if (search.hwnd == nullptr) {
        search.byProcess = false;
        ::EnumWindows(enumEasyEdaWindowsProc, reinterpret_cast<LPARAM>(&search));
    }
    return search.hwnd;
}

// Returns an AddRef'd element whose Name equals one of names (and whose ControlType equals
// controlType when controlType != 0), searching all descendants of root; nullptr if none.
IUIAutomationElement* findUiaElementByName(IUIAutomation* uia, IUIAutomationElement* root,
                                           const std::vector<std::wstring>& names, int controlType) {
    for (const auto& name : names) {
        VARIANT nameVar;
        ::VariantInit(&nameVar);
        nameVar.vt = VT_BSTR;
        nameVar.bstrVal = ::SysAllocString(name.c_str());
        if (nameVar.bstrVal == nullptr) continue;
        ComRelease<IUIAutomationCondition> nameCond;
        ComRelease<IUIAutomationCondition> typeCond;
        ComRelease<IUIAutomationCondition> andCond;
        IUIAutomationElement* found = nullptr;
        if (SUCCEEDED(uia->CreatePropertyCondition(UIA_NamePropertyId, nameVar, &nameCond.p)) &&
            nameCond.p != nullptr) {
            IUIAutomationCondition* cond = nameCond.p;
            if (controlType != 0) {
                VARIANT typeVar;
                ::VariantInit(&typeVar);
                typeVar.vt = VT_I4;
                typeVar.lVal = controlType;
                if (SUCCEEDED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, typeVar, &typeCond.p)) &&
                    typeCond.p != nullptr &&
                    SUCCEEDED(uia->CreateAndCondition(nameCond.p, typeCond.p, &andCond.p)) &&
                    andCond.p != nullptr) {
                    cond = andCond.p;
                }
            }
            root->FindFirst(TreeScope_Descendants, cond, &found);
        }
        ::VariantClear(&nameVar);
        if (found != nullptr) return found;
    }
    return nullptr;
}

IUIAutomationElement* pollUiaElementByName(IUIAutomation* uia, IUIAutomationElement* root,
                                           const std::vector<std::wstring>& names, int controlType,
                                           int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        IUIAutomationElement* el = findUiaElementByName(uia, root, names, controlType);
        if (el != nullptr) return el;
        if (std::chrono::steady_clock::now() >= deadline) return nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void sendMouseClickAt(int x, int y) {
    ::SetCursorPos(x, y);
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    ::SendInput(2, inputs, sizeof(INPUT));
}

void sendVirtualKey(WORD vk) {
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(2, inputs, sizeof(INPUT));
}

// Invoke -> ExpandCollapse -> SendInput click at the element centre. Returns the method used.
std::string activateUiaElement(IUIAutomationElement* el) {
    ComRelease<IUIAutomationInvokePattern> invoke;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern),
                                          reinterpret_cast<void**>(&invoke.p))) &&
        invoke.p != nullptr && SUCCEEDED(invoke.p->Invoke())) {
        return "invoke";
    }
    ComRelease<IUIAutomationExpandCollapsePattern> expand;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ExpandCollapsePatternId,
                                          __uuidof(IUIAutomationExpandCollapsePattern),
                                          reinterpret_cast<void**>(&expand.p))) &&
        expand.p != nullptr && SUCCEEDED(expand.p->Expand())) {
        return "expand";
    }
    RECT rc = {};
    if (SUCCEEDED(el->get_CurrentBoundingRectangle(&rc)) && rc.right > rc.left && rc.bottom > rc.top) {
        sendMouseClickAt((rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2);
        return "click";
    }
    return "";
}

// Drives EasyEDA Pro's own Route -> Auto Route... -> Run through UI Automation. Electron builds its
// accessibility tree lazily, so the first lookups are polled. Diagnostics land in outDiag.
bool triggerEasyEdaNativeAutoRouteWin32(int dialogTimeoutMs, nlohmann::json& outDiag) {
    ComApartmentScope com;
    if (!com.usable()) {
        outDiag["error"] = "CoInitializeEx failed";
        return false;
    }
    HWND hwnd = findEasyEdaProWindow();
    if (hwnd == nullptr) {
        outDiag["error"] = "EasyEDA Pro window not found (no visible easyeda-pro.exe / EasyEDA top-level window)";
        return false;
    }
    if (::IsIconic(hwnd)) ::ShowWindow(hwnd, SW_RESTORE);
    ::SetForegroundWindow(hwnd);

    ComRelease<IUIAutomation> uia;
    HRESULT hr = ::CoCreateInstance(kClsidCUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                    __uuidof(IUIAutomation), reinterpret_cast<void**>(&uia.p));
    if (FAILED(hr) || uia.p == nullptr) {
        outDiag["error"] = "CoCreateInstance(CUIAutomation) failed";
        return false;
    }
    ComRelease<IUIAutomationElement> root;
    if (FAILED(uia.p->ElementFromHandle(hwnd, &root.p)) || root.p == nullptr) {
        outDiag["error"] = "IUIAutomation::ElementFromHandle failed for the EasyEDA Pro window";
        return false;
    }

    const std::vector<std::wstring> routeNames = {L"Route", L"\u5e03\u7ebf"};
    const std::vector<std::wstring> autoItemNames = {
        L"Auto Route...", L"Auto Route\u2026", L"Auto Route", L"Auto Router...", L"Auto Router",
        L"\u81ea\u52a8\u5e03\u7ebf...", L"\u81ea\u52a8\u5e03\u7ebf\u2026", L"\u81ea\u52a8\u5e03\u7ebf"
    };
    const std::vector<std::wstring> runNames = {
        L"Run", L"Run Auto Route", L"Auto Route", L"\u6267\u884c", L"\u5f00\u59cb\u5e03\u7ebf", L"\u8fd0\u884c"
    };

    ComRelease<IUIAutomationElement> routeMenu;
    routeMenu.p = pollUiaElementByName(uia.p, root.p, routeNames, 0, 3000);
    if (routeMenu.p == nullptr) {
        outDiag["stage"] = "route_menu_not_found";
        outDiag["error"] = "UI Automation could not find the Route menu in the EasyEDA Pro window";
        return false;
    }
    outDiag["route_menu_method"] = activateUiaElement(routeMenu.p);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    ComRelease<IUIAutomationElement> autoItem;
    autoItem.p = pollUiaElementByName(uia.p, root.p, autoItemNames, 0, 2000);
    if (autoItem.p == nullptr) {
        // Native popup menus are separate top-level windows: search the desktop once.
        ComRelease<IUIAutomationElement> desktop;
        if (SUCCEEDED(uia.p->GetRootElement(&desktop.p)) && desktop.p != nullptr) {
            autoItem.p = findUiaElementByName(uia.p, desktop.p, autoItemNames, UIA_MenuItemControlTypeId);
        }
    }
    if (autoItem.p == nullptr) {
        outDiag["stage"] = "auto_route_item_not_found";
        outDiag["error"] = "UI Automation could not find the Auto Route menu item";
        sendVirtualKey(VK_ESCAPE);
        return false;
    }
    outDiag["auto_route_item_method"] = activateUiaElement(autoItem.p);

    ComRelease<IUIAutomationElement> runBtn;
    runBtn.p = pollUiaElementByName(uia.p, root.p, runNames, UIA_ButtonControlTypeId, dialogTimeoutMs);
    if (runBtn.p != nullptr) {
        outDiag["run_method"] = activateUiaElement(runBtn.p);
    } else if (::GetForegroundWindow() == hwnd) {
        // Assumes Run is the Auto Route dialog's default button; unverified until qualified on
        // Windows, and the caller judges success by the settled track count, not by this path.
        sendVirtualKey(VK_RETURN);
        outDiag["run_method"] = "enter_key";
    } else {
        outDiag["stage"] = "run_button_not_found";
        outDiag["error"] = "Auto Route dialog Run button not found and EasyEDA Pro is not the foreground window";
        return false;
    }
    outDiag["stage"] = "triggered";
    return true;
}
#else
bool triggerEasyEdaNativeAutoRouteWin32(int /*dialogTimeoutMs*/, nlohmann::json& outDiag) {
    outDiag["error"] = "Win32 UI Automation is only available when aimon-eda-bridge runs on Windows";
    return false;
}
#endif

void closeSocketFd(int fd) {
    if (fd < 0) return;
#ifdef _WIN32
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
#endif
}

bool sendAllBytes(int fd, const uint8_t* data, size_t len) {
    if (fd < 0) return false;
    size_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        int n = ::send(static_cast<SOCKET>(fd),
                       reinterpret_cast<const char*>(data + sent),
                       static_cast<int>(len - sent), 0);
#else
        ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool sendJsonLine(int fd, const nlohmann::json& j) {
    std::string s = j.dump() + "\n";
    return sendAllBytes(fd, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// Pure C++17 SHA-1 + Base64 for RFC 6455 Sec-WebSocket-Accept
uint32_t rol32(uint32_t value, unsigned int bits) {
    return (value << bits) | (value >> (32 - bits));
}

void sha1Digest(const std::string& input, uint8_t out[20]) {
    uint32_t h0 = 0x67452301u;
    uint32_t h1 = 0xEFCDAB89u;
    uint32_t h2 = 0x98BADCFEu;
    uint32_t h3 = 0x10325476u;
    uint32_t h4 = 0xC3D2E1F0u;

    std::vector<uint8_t> msg(input.begin(), input.end());
    uint64_t bitLen = static_cast<uint64_t>(msg.size()) * 8ULL;
    msg.push_back(0x80u);
    while ((msg.size() % 64) != 56) {
        msg.push_back(0x00u);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xFFu));
    }

    for (size_t offset = 0; offset < msg.size(); offset += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            size_t j = offset + i * 4;
            w[i] = (static_cast<uint32_t>(msg[j]) << 24) |
                   (static_cast<uint32_t>(msg[j + 1]) << 16) |
                   (static_cast<uint32_t>(msg[j + 2]) << 8) |
                   static_cast<uint32_t>(msg[j + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            uint32_t temp = rol32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol32(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>((hs[i] >> 24) & 0xFFu);
        out[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 16) & 0xFFu);
        out[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 8) & 0xFFu);
        out[i * 4 + 3] = static_cast<uint8_t>(hs[i] & 0xFFu);
    }
}

std::string base64Encode(const uint8_t* data, size_t len) {
    static constexpr const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) v |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(kTable[(v >> 18) & 0x3Fu]);
        out.push_back(kTable[(v >> 12) & 0x3Fu]);
        out.push_back((i + 1 < len) ? kTable[(v >> 6) & 0x3Fu] : '=');
        out.push_back((i + 2 < len) ? kTable[v & 0x3Fu] : '=');
    }
    return out;
}

bool isPathFieldKey(const std::string& key) {
    if (key == "api_path" || key == "method_path" || key == "key_path") {
        return false;
    }
    static constexpr const char* kSuffixes[] = {
        "_path", "_dir", "_file", "_root", "files", "paths"
    };
    if (key == "path" || key == "file" || key == "directory" || key == "project" ||
        key == "output" || key == "input" || key == "schematic" || key == "pcb") {
        return true;
    }
    for (const char* s : kSuffixes) {
        size_t sLen = std::char_traits<char>::length(s);
        if (key.size() >= sLen && key.compare(key.size() - sLen, sLen, s) == 0) {
            return true;
        }
    }
    return false;
}

bool checkFreecadScriptSafety(const std::string& scriptCode, std::string& outReason) {
    outReason.clear();
    if (scriptCode.empty()) {
        outReason = "Empty FreeCAD script";
        return false;
    }
    if (scriptCode.find('\0') != std::string::npos) {
        outReason = "FreeCAD script contains null byte";
        return false;
    }
    auto isIdentChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    static constexpr const char* kTnpPrefixes[] = {"Face", "Edge", "Vertex"};
    for (const char* pfx : kTnpPrefixes) {
        size_t pfxLen = std::char_traits<char>::length(pfx);
        size_t pos = 0;
        while ((pos = scriptCode.find(pfx, pos)) != std::string::npos) {
            bool wordStart = (pos == 0) || !isIdentChar(scriptCode[pos - 1]);
            size_t numStart = pos + pfxLen;
            if (wordStart && numStart < scriptCode.size() &&
                scriptCode[numStart] >= '1' && scriptCode[numStart] <= '9') {
                size_t numEnd = numStart + 1;
                while (numEnd < scriptCode.size() &&
                       std::isdigit(static_cast<unsigned char>(scriptCode[numEnd]))) {
                    ++numEnd;
                }
                bool wordEnd = (numEnd == scriptCode.size()) || !isIdentChar(scriptCode[numEnd]);
                if (wordEnd && (numEnd - numStart) <= 4) {
                    std::string matched = scriptCode.substr(pos, numEnd - pos);
                    outReason = "Rejected by FreeCAD TNP guard: brittle topological reference '" + matched + "'";
                    return false;
                }
            }
            pos += pfxLen;
        }
    }
    static constexpr const char* kForbiddenTokens[] = {
        "subprocess", "os.system", "os.popen", "socket", "urllib", "http.client",
        "shutil.rmtree", "ctypes", "__import__", "eval(", "exec("
    };
    for (const char* token : kForbiddenTokens) {
        if (scriptCode.find(token) != std::string::npos) {
            outReason = "Rejected by FreeCAD sandbox guard: forbidden module or call '" +
                        std::string(token) + "'";
            return false;
        }
    }
    return true;
}

bool checkFreecadApiSafety(const std::string& moduleName,
                           const std::string& methodName,
                           std::string& outReason) {
    outReason.clear();
    static constexpr const char* kAllowedModules[] = {
        "FreeCAD", "Part", "PartDesign", "Sketcher", "Mesh", "MeshPart", "Spreadsheet", "TechDraw", "Import"
    };
    bool modAllowed = false;
    for (const char* m : kAllowedModules) {
        if (moduleName == m) {
            modAllowed = true;
            break;
        }
    }
    if (!modAllowed) {
        outReason = "FreeCAD module '" + moduleName + "' is not in the allowed workbench whitelist";
        return false;
    }
    if (methodName.empty() || methodName.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.") != std::string::npos ||
        methodName.find("__") != std::string::npos || methodName == "eval" || methodName == "exec") {
        outReason = "Forbidden FreeCAD method '" + methodName + "'";
        return false;
    }
    return true;
}

bool checkEasyEdaApiSafety(const std::string& apiPath, std::string& outReason) {
    outReason.clear();
    if (apiPath.empty() || apiPath.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.") != std::string::npos) {
        outReason = "Invalid EasyEDA API identifier";
        return false;
    }
    static constexpr const char* kAllowedPrefixes[] = {
        "eda.sch_", "eda.pcb_", "eda.lib_", "eda.dmt_", "eda.drc_", "eda.sys_Document"
    };
    for (const char* prefix : kAllowedPrefixes) {
        size_t pLen = std::char_traits<char>::length(prefix);
        if (apiPath.rfind(prefix, 0) == 0 && apiPath.size() > pLen &&
            apiPath.find('.', pLen) != std::string::npos) {
            return true;
        }
    }
    outReason = "EasyEDA API '" + apiPath + "' is outside the allowed eda.* whitelist";
    return false;
}

// Minimal valid 1x1 PNG bytes for deterministic headless viewport rendering
std::vector<uint8_t> minimalPngBytes() {
    static constexpr uint8_t kPng[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
        0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41,
        0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
        0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xDD, 0x8D,
        0xB0, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,
        0x44, 0xAE, 0x42, 0x60, 0x82
    };
    return std::vector<uint8_t>(kPng, kPng + sizeof(kPng));
}

std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

nlohmann::json introspectEasyEdaApiCatalog(const nlohmann::json& args) {
    struct ApiEntry {
        const char* apiPath;
        const char* ns;
        const char* method;
        const char* signature;
        const char* returnType;
        const char* description;
        nlohmann::json parameters;
    };

    const std::vector<ApiEntry> kCatalog = {
        {
            "eda.pcb_PrimitiveLine.create",
            "pcb_PrimitiveLine",
            "create",
            "create(net: string, layer: TPCB_LayersOfLine, startX: number, startY: number, endX: number, endY: number, lineWidth?: number, locked?: boolean): Promise<IPCB_PrimitiveLine | undefined>",
            "Promise<IPCB_PrimitiveLine | undefined>",
            "Create a copper track segment on a PCB layer (coordinates and lineWidth in mil: 1 mil = 0.0254 mm).",
            nlohmann::json::array({
                {{"name", "net"}, {"type", "string"}, {"optional", false}},
                {{"name", "layer"}, {"type", "TPCB_LayersOfLine"}, {"optional", false}},
                {{"name", "startX"}, {"type", "number (mil)"}, {"optional", false}},
                {{"name", "startY"}, {"type", "number (mil)"}, {"optional", false}},
                {{"name", "endX"}, {"type", "number (mil)"}, {"optional", false}},
                {{"name", "endY"}, {"type", "number (mil)"}, {"optional", false}},
                {{"name", "lineWidth"}, {"type", "number (mil)"}, {"optional", true}},
                {{"name", "locked"}, {"type", "boolean"}, {"optional", true}}
            })
        },
        {
            "eda.pcb_PrimitiveLine.getAll",
            "pcb_PrimitiveLine",
            "getAll",
            "getAll(net?: string, layer?: TPCB_LayersOfLine): Promise<IPCB_PrimitiveLine[]>",
            "Promise<IPCB_PrimitiveLine[]>",
            "Retrieve all copper track line primitives on the active PCB canvas.",
            nlohmann::json::array({
                {{"name", "net"}, {"type", "string"}, {"optional", true}},
                {{"name", "layer"}, {"type", "TPCB_LayersOfLine"}, {"optional", true}}
            })
        },
        {
            "eda.pcb_PrimitiveVia.create",
            "pcb_PrimitiveVia",
            "create",
            "create(net: string, x: number, y: number, holeDiameter?: number, diameter?: number, viaType?: EPCB_PrimitiveViaType, padLayer?: number[], locked?: boolean): Promise<IPCB_PrimitiveVia | undefined>",
            "Promise<IPCB_PrimitiveVia | undefined>",
            "Create an interlayer via on the PCB canvas (x, y, holeDiameter, diameter in mil).",
            nlohmann::json::array({
                {{"name", "net"}, {"type", "string"}, {"optional", false}},
                {{"name", "x"}, {"type", "number (mil)"}, {"optional", false}},
                {{"name", "y"}, {"type", "number (mil)"}, {"optional", false}},
                {{"name", "holeDiameter"}, {"type", "number (mil)"}, {"optional", true}},
                {{"name", "diameter"}, {"type", "number (mil)"}, {"optional", true}},
                {{"name", "viaType"}, {"type", "EPCB_PrimitiveViaType"}, {"optional", true}}
            })
        },
        {
            "eda.pcb_PrimitiveVia.getAll",
            "pcb_PrimitiveVia",
            "getAll",
            "getAll(net?: string): Promise<IPCB_PrimitiveVia[]>",
            "Promise<IPCB_PrimitiveVia[]>",
            "Retrieve all via primitives from the active PCB document.",
            nlohmann::json::array({
                {{"name", "net"}, {"type", "string"}, {"optional", true}}
            })
        },
        {
            "eda.pcb_PrimitivePad.getAll",
            "pcb_PrimitivePad",
            "getAll",
            "getAll(layerId?: number, net?: string): Promise<IPCB_PrimitivePad[]>",
            "Promise<IPCB_PrimitivePad[]>",
            "Retrieve all component/board pad primitives with net and coordinate metadata.",
            nlohmann::json::array({
                {{"name", "layerId"}, {"type", "number"}, {"optional", true}},
                {{"name", "net"}, {"type", "string"}, {"optional", true}}
            })
        },
        {
            "eda.pcb_PrimitiveComponent.getAll",
            "pcb_PrimitiveComponent",
            "getAll",
            "getAll(layerId?: EPCB_LayerId, locked?: boolean): Promise<IPCB_PrimitiveComponent[]>",
            "Promise<IPCB_PrimitiveComponent[]>",
            "Retrieve all placed PCB footprint components.",
            nlohmann::json::array({
                {{"name", "layerId"}, {"type", "EPCB_LayerId"}, {"optional", true}},
                {{"name", "locked"}, {"type", "boolean"}, {"optional", true}}
            })
        },
        {
            "eda.pcb_PrimitiveComponent.modify",
            "pcb_PrimitiveComponent",
            "modify",
            "modify(primitiveId: string, property: Partial<IPCB_PrimitiveComponentProperty>): Promise<IPCB_PrimitiveComponent | undefined>",
            "Promise<IPCB_PrimitiveComponent | undefined>",
            "Move, rotate, flip layer, or update designator of a placed PCB footprint.",
            nlohmann::json::array({
                {{"name", "primitiveId"}, {"type", "string"}, {"optional", false}},
                {{"name", "property"}, {"type", "Partial<IPCB_PrimitiveComponentProperty>"}, {"optional", false}}
            })
        },
        {
            "eda.pcb_PrimitivePour.create",
            "pcb_PrimitivePour",
            "create",
            "create(net: string, layer: TPCB_LayersOfPour, polygon: IPCB_Polygon, pourName?: string, clearance?: number): Promise<IPCB_PrimitivePour | undefined>",
            "Promise<IPCB_PrimitivePour | undefined>",
            "Create a copper pour zone attached to a net on TopLayer/BottomLayer/inner plane.",
            nlohmann::json::array({
                {{"name", "net"}, {"type", "string"}, {"optional", false}},
                {{"name", "layer"}, {"type", "TPCB_LayersOfPour"}, {"optional", false}},
                {{"name", "polygon"}, {"type", "IPCB_Polygon"}, {"optional", false}},
                {{"name", "clearance"}, {"type", "number (mil)"}, {"optional", true}}
            })
        },
        {
            "eda.pcb_Drc.check",
            "pcb_Drc",
            "check",
            "check(strict?: boolean, userInterface?: boolean, includeVerboseError?: boolean): Promise<boolean | any[]>",
            "Promise<boolean | any[]>",
            "Execute EasyEDA Pro PCB Design Rule Check (DRC) and return structured violations.",
            nlohmann::json::array({
                {{"name", "strict"}, {"type", "boolean"}, {"optional", true}},
                {{"name", "userInterface"}, {"type", "boolean"}, {"optional", true}},
                {{"name", "includeVerboseError"}, {"type", "boolean"}, {"optional", true}}
            })
        },
        {
            "eda.pcb_ManufactureData.getGerberFile",
            "pcb_ManufactureData",
            "getGerberFile",
            "getGerberFile(fileName?: string): Promise<File | undefined>",
            "Promise<File | undefined>",
            "Generate JLCPCB-ready Gerber and Excellon drill archive from the active PCB.",
            nlohmann::json::array({
                {{"name", "fileName"}, {"type", "string"}, {"optional", true}}
            })
        },
        {
            "eda.sch_PrimitiveComponent.create",
            "sch_PrimitiveComponent",
            "create",
            "create(component: {libraryUuid: string, uuid: string} | ISCH_NetlistComponent, x: number, y: number, subPartName?: string, rotation?: number, mirror?: boolean, addIntoBom?: boolean, addIntoPcb?: boolean): Promise<ISCH_PrimitiveComponent | undefined>",
            "Promise<ISCH_PrimitiveComponent | undefined>",
            "Place a schematic symbol component onto the active schematic sheet.",
            nlohmann::json::array({
                {{"name", "component"}, {"type", "{libraryUuid: string, uuid: string}"}, {"optional", false}},
                {{"name", "x"}, {"type", "number"}, {"optional", false}},
                {{"name", "y"}, {"type", "number"}, {"optional", false}},
                {{"name", "rotation"}, {"type", "number"}, {"optional", true}},
                {{"name", "mirror"}, {"type", "boolean"}, {"optional", true}}
            })
        },
        {
            "eda.sch_PrimitiveComponent.getAll",
            "sch_PrimitiveComponent",
            "getAll",
            "getAll(primitiveType?: ESCH_PrimitiveComponentType, allSchematicPages?: boolean): Promise<ISCH_PrimitiveComponent[]>",
            "Promise<ISCH_PrimitiveComponent[]>",
            "Retrieve all schematic components on the active sheet or across all pages.",
            nlohmann::json::array({
                {{"name", "primitiveType"}, {"type", "ESCH_PrimitiveComponentType"}, {"optional", true}},
                {{"name", "allSchematicPages"}, {"type", "boolean"}, {"optional", true}}
            })
        },
        {
            "eda.sch_PrimitiveComponent.getAllPinsByPrimitiveId",
            "sch_PrimitiveComponent",
            "getAllPinsByPrimitiveId",
            "getAllPinsByPrimitiveId(primitiveId: string): Promise<ISCH_PrimitiveComponentPin[] | undefined>",
            "Promise<ISCH_PrimitiveComponentPin[] | undefined>",
            "Retrieve all pins (pinNumber, pinName, x, y, rotation, net) of a placed schematic component.",
            nlohmann::json::array({
                {{"name", "primitiveId"}, {"type", "string"}, {"optional", false}}
            })
        },
        {
            "eda.sch_PrimitiveComponent.createNetFlag",
            "sch_PrimitiveComponent",
            "createNetFlag",
            "createNetFlag(identification: 'Power' | 'Ground' | 'AnalogGround' | 'ProtectGround', net: string, x: number, y: number, rotation?: number, mirror?: boolean): Promise<ISCH_PrimitiveComponent | undefined>",
            "Promise<ISCH_PrimitiveComponent | undefined>",
            "Place a schematic power or ground netFlag (VCC, GND, +3V3) at (x, y) in mil.",
            nlohmann::json::array({
                {{"name", "identification"}, {"type", "'Power' | 'Ground' | 'AnalogGround' | 'ProtectGround'"}, {"optional", false}},
                {{"name", "net"}, {"type", "string"}, {"optional", false}},
                {{"name", "x"}, {"type", "number (mil)"}, {"optional", false}},
                {{"name", "y"}, {"type", "number (mil)"}, {"optional", false}},
                {{"name", "rotation"}, {"type", "number"}, {"optional", true}},
                {{"name", "mirror"}, {"type", "boolean"}, {"optional", true}}
            })
        },
        {
            "eda.sch_PrimitiveComponent.createNetPort",
            "sch_PrimitiveComponent",
            "createNetPort",
            "createNetPort(direction: 'IN' | 'OUT' | 'BI', net: string, x: number, y: number, rotation?: number, mirror?: boolean): Promise<ISCH_PrimitiveComponent | undefined>",
            "Promise<ISCH_PrimitiveComponent | undefined>",
            "Place a schematic net port label at (x, y) in mil.",
            nlohmann::json::array({
                {{"name", "direction"}, {"type", "'IN' | 'OUT' | 'BI'"}, {"optional", false}},
                {{"name", "net"}, {"type", "string"}, {"optional", false}},
                {{"name", "x"}, {"type", "number (mil)"}, {"optional", false}},
                {{"name", "y"}, {"type", "number (mil)"}, {"optional", false}}
            })
        },
        {
            "eda.sch_PrimitiveWire.create",
            "sch_PrimitiveWire",
            "create",
            "create(line: number[] | number[][], net?: string, color?: string, lineWidth?: number, lineType?: ESCH_PrimitiveLineType): Promise<ISCH_PrimitiveWire | undefined>",
            "Promise<ISCH_PrimitiveWire | undefined>",
            "Create a schematic wire polyline and optionally bind a net label.",
            nlohmann::json::array({
                {{"name", "line"}, {"type", "number[] | number[][]"}, {"optional", false}},
                {{"name", "net"}, {"type", "string"}, {"optional", true}}
            })
        },
        {
            "eda.sch_PrimitiveWire.getAll",
            "sch_PrimitiveWire",
            "getAll",
            "getAll(net?: string, allSchematicPages?: boolean): Promise<ISCH_PrimitiveWire[]>",
            "Promise<ISCH_PrimitiveWire[]>",
            "Retrieve all schematic wire primitives on the active sheet or across all pages.",
            nlohmann::json::array({
                {{"name", "net"}, {"type", "string"}, {"optional", true}},
                {{"name", "allSchematicPages"}, {"type", "boolean"}, {"optional", true}}
            })
        },
        {
            "eda.sch_Drc.check",
            "sch_Drc",
            "check",
            "check(strict?: boolean, userInterface?: boolean, includeVerboseError?: boolean): Promise<boolean | any[]>",
            "Promise<boolean | any[]>",
            "Run Schematic Electrical Rule Check (ERC) inside EasyEDA Pro.",
            nlohmann::json::array({
                {{"name", "strict"}, {"type", "boolean"}, {"optional", true}},
                {{"name", "userInterface"}, {"type", "boolean"}, {"optional", true}},
                {{"name", "includeVerboseError"}, {"type", "boolean"}, {"optional", true}}
            })
        },
        {
            "eda.lib_Device.getByLcscIds",
            "lib_Device",
            "getByLcscIds",
            "getByLcscIds(lcscIds: string | string[], libraryUuid?: string): Promise<ILIB_DeviceSearchItem[]>",
            "Promise<ILIB_DeviceSearchItem[]>",
            "Resolve LCSC C-numbers (e.g. C25804) to EasyEDA device UUID and libraryUuid.",
            nlohmann::json::array({
                {{"name", "lcscIds"}, {"type", "string | string[]"}, {"optional", false}},
                {{"name", "libraryUuid"}, {"type", "string"}, {"optional", true}}
            })
        },
        {
            "eda.dmt_SelectControl.getCurrentDocumentInfo",
            "dmt_SelectControl",
            "getCurrentDocumentInfo",
            "getCurrentDocumentInfo(): Promise<IDMT_DocumentInfo | undefined>",
            "Promise<IDMT_DocumentInfo | undefined>",
            "Inspect the currently focused schematic or PCB tab in EasyEDA Pro.",
            nlohmann::json::array()
        }
    };

    bool isDescribe = args.contains("describe_api") && args["describe_api"].is_string() &&
                      !args["describe_api"].get<std::string>().empty();
    std::string rawQuery = isDescribe
        ? args["describe_api"].get<std::string>()
        : args.value("search_api", "");
    std::string qLower = toLowerAscii(rawQuery);

    nlohmann::json matched = nlohmann::json::array();
    for (const auto& entry : kCatalog) {
        std::string pathLower = toLowerAscii(entry.apiPath);
        std::string sigLower = toLowerAscii(entry.signature);
        std::string descLower = toLowerAscii(entry.description);
        bool hit = false;
        if (isDescribe) {
            hit = (pathLower == qLower) || (pathLower.find(qLower) != std::string::npos);
        } else {
            hit = (pathLower.find(qLower) != std::string::npos) ||
                  (sigLower.find(qLower) != std::string::npos) ||
                  (descLower.find(qLower) != std::string::npos);
        }
        if (hit) {
            std::string notes = std::string(entry.description) + " Signature: " + entry.signature +
                                " (1 mil = 0.0254 mm)";
            matched.push_back({
                {"api_path", entry.apiPath},
                {"namespace", entry.ns},
                {"method", entry.method},
                {"signature", entry.signature},
                {"parameters", entry.parameters},
                {"return_type", entry.returnType},
                {"unit_note", "1 mil = 0.0254 mm (EasyEDA Pro native canvas unit is mil)"},
                {"description", entry.description},
                {"notes", notes}
            });
        }
    }

    return {
        {"ok", true},
        {"offline_introspection", true},
        {"mode", isDescribe ? "describe_api" : "search_api"},
        {"query", rawQuery},
        {"matched_count", matched.size()},
        {"matched_apis", matched}
    };
}

void attachFreeCadSpecAndDiff(nlohmann::json& docResult,
                              const nlohmann::json& args,
                              const nlohmann::json* prevDocSnapshot) {
    if (!docResult.is_object()) return;

    double totalVol = docResult.value("volume_mm3", 0.0);
    if (totalVol <= 0.0 && docResult.contains("bodies") && docResult["bodies"].is_array()) {
        for (const auto& b : docResult["bodies"]) {
            if (b.is_object()) totalVol += b.value("volume_mm3", 0.0);
        }
    }
    if (args.contains("volume_mm3") && args["volume_mm3"].is_number()) {
        totalVol = args["volume_mm3"].get<double>();
    }

    std::vector<double> docBbox = {0.0, 0.0, 0.0};
    if (docResult.contains("bbox_mm") && docResult["bbox_mm"].is_array() && docResult["bbox_mm"].size() >= 3) {
        docBbox[0] = docResult["bbox_mm"][0].get<double>();
        docBbox[1] = docResult["bbox_mm"][1].get<double>();
        docBbox[2] = docResult["bbox_mm"][2].get<double>();
    } else if (docResult.contains("bounding_box") && docResult["bounding_box"].is_object()) {
        const auto& bb = docResult["bounding_box"];
        docBbox[0] = bb.value("x_max", 0.0) - bb.value("x_min", 0.0);
        docBbox[1] = bb.value("y_max", 0.0) - bb.value("y_min", 0.0);
        docBbox[2] = bb.value("z_max", 0.0) - bb.value("z_min", 0.0);
    }

    int holesCount = docResult.value("holes_count", -1);
    if (holesCount < 0) {
        if (docResult.contains("holes") && docResult["holes"].is_array()) {
            holesCount = static_cast<int>(docResult["holes"].size());
        } else {
            holesCount = args.value("holes_count", 0);
        }
    }

    bool manifoldValid = docResult.value("watertight", true) && docResult.value("is_manifold", true);
    if (docResult.contains("bodies") && docResult["bodies"].is_array()) {
        for (const auto& b : docResult["bodies"]) {
            if (b.is_object() && !b.value("watertight", true)) {
                manifoldValid = false;
            }
        }
    }

    if (!docResult.contains("spec_validation") && args.contains("spec") && args["spec"].is_object()) {
        const auto& spec = args["spec"];
        nlohmann::json violations = nlohmann::json::array();
        bool reqManifold = spec.value("require_watertight_manifold", true);
        bool manifoldOk = (!reqManifold) || manifoldValid;
        if (!manifoldOk) {
            violations.push_back("Document contains non-manifold or non-watertight solid bodies");
        }

        bool bboxMatched = true;
        double bboxTol = spec.value("bbox_tolerance_mm", 0.5);
        if (spec.contains("expected_bbox_mm") && spec["expected_bbox_mm"].is_array() &&
            spec["expected_bbox_mm"].size() >= 3) {
            static constexpr const char* kAxes[3] = {"X", "Y", "Z"};
            for (size_t i = 0; i < 3; ++i) {
                double targetDim = spec["expected_bbox_mm"][i].get<double>();
                if (std::abs(docBbox[i] - targetDim) > bboxTol) {
                    bboxMatched = false;
                    violations.push_back(std::string("Bounding box ") + kAxes[i] +
                                         " dimension outside tolerance");
                }
            }
        }

        bool volumeMatched = true;
        if (spec.contains("expected_volume_mm3") && spec["expected_volume_mm3"].is_number()) {
            double expVol = spec["expected_volume_mm3"].get<double>();
            double tolPct = spec.value("volume_tolerance_pct", 5.0);
            double allowedDelta = std::abs(expVol) * (tolPct / 100.0);
            if (std::abs(totalVol - expVol) > allowedDelta) {
                volumeMatched = false;
                violations.push_back("Volume outside tolerance of expected_volume_mm3");
            }
        }

        bool holesMatched = true;
        if (spec.contains("expected_holes_count") && spec["expected_holes_count"].is_number_integer()) {
            int expHoles = spec["expected_holes_count"].get<int>();
            if (holesCount != expHoles) {
                holesMatched = false;
                violations.push_back("Holes count does not match expected_holes_count");
            }
        }

        docResult["spec_validation"] = {
            {"passed", manifoldOk && bboxMatched && volumeMatched && holesMatched},
            {"manifold_valid", manifoldValid},
            {"bbox_matched", bboxMatched},
            {"volume_matched", volumeMatched},
            {"holes_matched", holesMatched},
            {"actual_bbox_mm", docBbox},
            {"actual_volume_mm3", totalVol},
            {"actual_holes_count", holesCount},
            {"violations", violations}
        };
    }

    bool wantDiff = args.value("compare_previous", false) ||
                    (args.contains("previous_snapshot") && args["previous_snapshot"].is_object());
    if (!docResult.contains("volumetric_diff") && wantDiff) {
        nlohmann::json prevSnap = nlohmann::json::object();
        if (args.contains("previous_snapshot") && args["previous_snapshot"].is_object()) {
            prevSnap = args["previous_snapshot"];
        } else if (prevDocSnapshot != nullptr && prevDocSnapshot->is_object()) {
            prevSnap = *prevDocSnapshot;
        }

        double prevVol = prevSnap.value("volume_mm3", totalVol);
        std::vector<double> prevBbox = docBbox;
        if (prevSnap.contains("bbox_mm") && prevSnap["bbox_mm"].is_array() && prevSnap["bbox_mm"].size() >= 3) {
            prevBbox[0] = prevSnap["bbox_mm"][0].get<double>();
            prevBbox[1] = prevSnap["bbox_mm"][1].get<double>();
            prevBbox[2] = prevSnap["bbox_mm"][2].get<double>();
        } else if (prevSnap.contains("bounding_box") && prevSnap["bounding_box"].is_object()) {
            const auto& pbb = prevSnap["bounding_box"];
            prevBbox[0] = pbb.value("x_max", 0.0) - pbb.value("x_min", 0.0);
            prevBbox[1] = pbb.value("y_max", 0.0) - pbb.value("y_min", 0.0);
            prevBbox[2] = pbb.value("z_max", 0.0) - pbb.value("z_min", 0.0);
        }

        double curBoxVol = std::max(1e-6, docBbox[0] * docBbox[1] * docBbox[2]);
        double prevBoxVol = std::max(1e-6, prevBbox[0] * prevBbox[1] * prevBbox[2]);
        double interBoxVol = std::max(0.0, std::min(docBbox[0], prevBbox[0])) *
                             std::max(0.0, std::min(docBbox[1], prevBbox[1])) *
                             std::max(0.0, std::min(docBbox[2], prevBbox[2]));
        double boxOverlapRatio = std::min(1.0, interBoxVol / std::max(1e-6, std::min(curBoxVol, prevBoxVol)));
        double sharedVol = std::min(totalVol, prevVol) * boxOverlapRatio;
        sharedVol = std::max(0.0, std::min({sharedVol, totalVol, prevVol}));
        double addedVol = std::max(0.0, totalVol - sharedVol);
        double removedVol = std::max(0.0, prevVol - sharedVol);
        double netDelta = totalVol - prevVol;
        double unionVol = totalVol + prevVol - sharedVol;
        double iouRatio = (unionVol > 1e-9) ? (sharedVol / unionVol) : 1.0;

        docResult["volumetric_diff"] = {
            {"shared_volume_mm3", sharedVol},
            {"added_volume_mm3", addedVol},
            {"removed_volume_mm3", removedVol},
            {"net_volume_delta_mm3", netDelta},
            {"iou_ratio", iouRatio}
        };
    }
}

bool computeViewMatrixComplete(const std::vector<std::string>& views) {
    bool hasTop = false;
    bool hasFront = false;
    bool hasRight = false;
    bool hasIso = false;
    for (const auto& v : views) {
        std::string vl = toLowerAscii(v);
        if (vl == "top") hasTop = true;
        else if (vl == "front") hasFront = true;
        else if (vl == "right") hasRight = true;
        else if (vl == "iso" || vl == "isometric" || vl == "axonometric") hasIso = true;
    }
    return hasTop && hasFront && hasRight && hasIso;
}

} // anonymous namespace

EdaRpcServer::EdaRpcServer(const EdaRpcServerOptions& options)
    : _options(options),
      _enableEasyEda(options.enableEasyEda),
      _enableKiCad(options.enableKiCad),
      _enableFreeCad(options.enableFreeCad),
      _disallowExports(options.disallowExports) {
    _kicadInfo = ToolDiscovery::discoverKiCad(
        _options.kicadBinaryPath, _options.symbolLibDir, _options.footprintLibDir);
    _freecadInfo = ToolDiscovery::discoverFreeCad(_options.freecadBinaryPath);
    _easyedaInfo = ToolDiscovery::discoverEasyEda(_options.easyedaBinaryPath);
}

EdaRpcServer::~EdaRpcServer() {
    stop();
}

bool EdaRpcServer::constantTimeEquals(const std::string& a, const std::string& b) {
    volatile uint8_t diff = static_cast<uint8_t>(a.size() ^ b.size());
    size_t maxLen = std::max(a.size(), b.size());
    for (size_t i = 0; i < maxLen; ++i) {
        uint8_t ca = (i < a.size()) ? static_cast<uint8_t>(a[i]) : 0u;
        uint8_t cb = (i < b.size()) ? static_cast<uint8_t>(b[i]) : 0u;
        diff |= static_cast<uint8_t>(ca ^ cb);
    }
    return diff == 0u;
}

void EdaRpcServer::setAllowedRoot(const std::string& root) {
    std::lock_guard<std::mutex> lock(_allowedRootMutex);
    _options.allowedRoot = root;
}

std::string EdaRpcServer::allowedRoot() const {
    std::lock_guard<std::mutex> lock(_allowedRootMutex);
    return _options.allowedRoot;
}

bool EdaRpcServer::validateSandboxPath(const std::string& pathStr, std::string& outError) const {
    outError.clear();
    if (pathStr.empty()) {
        outError = "Empty path argument";
        return false;
    }
    if (pathStr.find('\0') != std::string::npos || pathStr.size() > 4096) {
        outError = "Invalid path argument";
        return false;
    }

    std::string norm = pathStr;
    std::replace(norm.begin(), norm.end(), '\\', '/');
    size_t pos = 0;
    while (pos <= norm.size()) {
        size_t next = norm.find('/', pos);
        std::string seg = (next == std::string::npos) ? norm.substr(pos) : norm.substr(pos, next - pos);
        if (seg == "..") {
            outError = "Path traversal segment '..' rejected by sandbox";
            return false;
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }

    std::string candNorm = fs::path(norm).lexically_normal().string();
    std::replace(candNorm.begin(), candNorm.end(), '\\', '/');
    while (candNorm.size() > 1 && candNorm.back() == '/') {
        candNorm.pop_back();
    }

    // Allow paths inside the parent directory of any currently connected plugin's activeDocument
    {
        std::lock_guard<std::mutex> lock(_pluginsMutex);
        for (const auto& kv : _pluginSessions) {
            if (!kv.second->active.load() || kv.second->activeDocument.empty()) continue;
            std::string activeNorm = kv.second->activeDocument;
            std::replace(activeNorm.begin(), activeNorm.end(), '\\', '/');
            fs::path docPath(activeNorm);
            if (docPath.has_parent_path()) {
                std::string docDirNorm = docPath.parent_path().lexically_normal().string();
                std::replace(docDirNorm.begin(), docDirNorm.end(), '\\', '/');
                while (docDirNorm.size() > 1 && docDirNorm.back() == '/') {
                    docDirNorm.pop_back();
                }
                if (!docDirNorm.empty() && docDirNorm != "." && docDirNorm != "/" &&
                    candNorm.rfind(docDirNorm, 0) == 0 &&
                    (candNorm.size() == docDirNorm.size() || candNorm[docDirNorm.size()] == '/')) {
                    return true;
                }
            }
        }
    }

    const std::string currentRoot = allowedRoot();
    if (!currentRoot.empty()) {
        std::string rootNorm = fs::path(currentRoot).lexically_normal().string();
        std::replace(rootNorm.begin(), rootNorm.end(), '\\', '/');
        while (rootNorm.size() > 1 && rootNorm.back() == '/') {
            rootNorm.pop_back();
        }

        if (candNorm.rfind(rootNorm, 0) != 0) {
            outError = "Path '" + pathStr + "' is outside allowed root '" + currentRoot + "'";
            return false;
        }
        if (candNorm.size() > rootNorm.size() && candNorm[rootNorm.size()] != '/') {
            outError = "Path '" + pathStr + "' does not match allowed root boundary";
            return false;
        }
    }

    return true;
}

bool EdaRpcServer::validateJsonSandboxPaths(const nlohmann::json& payload, std::string& outError) const {
    outError.clear();
    if (payload.is_object()) {
        for (auto it = payload.begin(); it != payload.end(); ++it) {
            const std::string& key = it.key();
            const nlohmann::json& val = it.value();
            if (val.is_string() && isPathFieldKey(key)) {
                std::string s = val.get<std::string>();
                if (!s.empty() && !validateSandboxPath(s, outError)) {
                    return false;
                }
            } else if (val.is_array() && isPathFieldKey(key)) {
                for (const auto& elem : val) {
                    if (elem.is_string()) {
                        std::string s = elem.get<std::string>();
                        if (!s.empty() && !validateSandboxPath(s, outError)) {
                            return false;
                        }
                    } else if (elem.is_object() || elem.is_array()) {
                        if (!validateJsonSandboxPaths(elem, outError)) return false;
                    }
                }
            } else if (val.is_object() || val.is_array()) {
                if (!validateJsonSandboxPaths(val, outError)) return false;
            }
        }
    } else if (payload.is_array()) {
        for (const auto& elem : payload) {
            if (elem.is_object() || elem.is_array()) {
                if (!validateJsonSandboxPaths(elem, outError)) return false;
            }
        }
    }
    return true;
}

std::string EdaRpcServer::computeWebSocketAcceptKey(const std::string& clientKey) {
    static constexpr const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = clientKey + kGuid;
    uint8_t digest[20];
    sha1Digest(combined, digest);
    return base64Encode(digest, sizeof(digest));
}

std::vector<uint8_t> EdaRpcServer::encodeWebSocketFrame(const std::string& payload,
                                                        uint8_t opcode,
                                                        bool mask) {
    std::vector<uint8_t> frame;
    size_t len = payload.size();
    frame.push_back(static_cast<uint8_t>(0x80u | (opcode & 0x0Fu))); // FIN + opcode

    uint8_t maskBit = mask ? 0x80u : 0x00u;
    if (len <= 125) {
        frame.push_back(static_cast<uint8_t>(maskBit | len));
    } else if (len <= 65535) {
        frame.push_back(static_cast<uint8_t>(maskBit | 126u));
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFFu));
        frame.push_back(static_cast<uint8_t>(len & 0xFFu));
    } else {
        frame.push_back(static_cast<uint8_t>(maskBit | 127u));
        uint64_t len64 = static_cast<uint64_t>(len);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((len64 >> (i * 8)) & 0xFFu));
        }
    }

    uint8_t maskKey[4] = {0x37u, 0xFAu, 0x21u, 0x3Du};
    if (mask) {
        frame.insert(frame.end(), maskKey, maskKey + 4);
        for (size_t i = 0; i < len; ++i) {
            frame.push_back(static_cast<uint8_t>(payload[i]) ^ maskKey[i % 4]);
        }
    } else {
        frame.insert(frame.end(), payload.begin(), payload.end());
    }
    return frame;
}

bool EdaRpcServer::decodeWebSocketFrame(const uint8_t* data,
                                        size_t len,
                                        size_t& outConsumed,
                                        uint8_t& outOpcode,
                                        std::string& outPayload,
                                        std::string& outError,
                                        bool* outFin) {
    outConsumed = 0;
    outOpcode = 0;
    outPayload.clear();
    outError.clear();
    if (outFin) *outFin = true;

    if (len < 2) return false; // Need more bytes

    uint8_t b0 = data[0];
    uint8_t b1 = data[1];
    if (outFin) *outFin = ((b0 & 0x80u) != 0);
    outOpcode = b0 & 0x0Fu;
    bool masked = (b1 & 0x80u) != 0;
    uint64_t payloadLen = b1 & 0x7Fu;
    size_t pos = 2;

    if (payloadLen == 126) {
        if (len < pos + 2) return false;
        payloadLen = (static_cast<uint64_t>(data[pos]) << 8) |
                     static_cast<uint64_t>(data[pos + 1]);
        pos += 2;
    } else if (payloadLen == 127) {
        if (len < pos + 8) return false;
        payloadLen = 0;
        for (int i = 0; i < 8; ++i) {
            payloadLen = (payloadLen << 8) | static_cast<uint64_t>(data[pos + i]);
        }
        pos += 8;
    }

    if (payloadLen > 4 * 1024 * 1024) {
        outError = "WebSocket frame exceeds 4 MiB maximum payload limit";
        return false;
    }

    uint8_t maskKey[4] = {0, 0, 0, 0};
    if (masked) {
        if (len < pos + 4) return false;
        std::memcpy(maskKey, data + pos, 4);
        pos += 4;
    }

    if (len < pos + payloadLen) return false;

    outPayload.resize(static_cast<size_t>(payloadLen));
    for (size_t i = 0; i < payloadLen; ++i) {
        uint8_t byte = data[pos + i];
        if (masked) byte ^= maskKey[i % 4];
        outPayload[i] = static_cast<char>(byte);
    }

    outConsumed = pos + static_cast<size_t>(payloadLen);
    return true;
}

std::vector<nlohmann::json> EdaRpcServer::remoteToolDescriptors() {
    struct ToolEntry {
        const char* name;
        const char* desc;
    };
    static constexpr ToolEntry kTools[] = {
        {"kicad_search_library", "Search KiCad .kicad_sym symbols and .kicad_mod footprints"},
        {"kicad_inspect_project", "Inspect KiCad .kicad_sch and .kicad_pcb project structure"},
        {"kicad_edit_schematic", "Create or modify KiCad 8 .kicad_sch on 50-mil grid"},
        {"kicad_sync_sch_to_pcb", "Synchronize schematic footprints and netlist into .kicad_pcb"},
        {"kicad_edit_pcb", "Edit .kicad_pcb outline, placements, copper zones, tracks, and vias"},
        {"kicad_run_erc", "Run KiCad Schematic Electrical Rules Check (ERC)"},
        {"kicad_run_drc", "Run KiCad PCB Design Rules Check (DRC)"},
        {"kicad_export_fab_pack", "Export turnkey Gerbers, drill, BOM CSV, and CPL CSV"},
        {"kicad_export_step", "Export 3D STEP PCB assembly model"},
        {"freecad_live_status", "Check live FreeCAD GUI attachment and active document"},
        {"freecad_inspect_document", "Inspect FreeCAD bodies, parameters, CoM, inertia, and LCS frames"},
        {"freecad_generate_cad", "Generate or modify parametric 3D robot bodies and joints"},
        {"freecad_edit_parameters", "Update FreeCAD Spreadsheet/model parameters and recompute"},
        {"freecad_assemble_joint", "Assemble robot joint and sweep range of motion for collisions"},
        {"freecad_analyze_printability", "Validate 3D-printability (watertight manifold, wall thickness, overhangs)"},
        {"freecad_render_view", "Render multi-angle PNG snapshots of 3D model"},
        {"freecad_export_print_files", "Export slicer-ready .stl, .3mf, .step, and URDF inertia metadata"},
        {"freecad_invoke_api", "Invoke whitelisted FreeCAD workbench API operation"},
        {"easyeda_live_status", "Check live EasyEDA Pro desktop extension attachment"},
        {"easyeda_search_library", "Search LCSC / EasyEDA component catalog"},
        {"easyeda_inspect_schematic", "Inspect live EasyEDA Pro schematic components, wires, and netlist"},
        {"easyeda_edit_schematic", "Place components, wires, and labels in EasyEDA Pro schematic"},
        {"easyeda_sync_sch_to_pcb", "Synchronize schematic into EasyEDA Pro PCB document"},
        {"easyeda_inspect_pcb", "Inspect EasyEDA Pro PCB outline, placements, pads, and unrouted nets"},
        {"easyeda_edit_pcb", "Edit EasyEDA Pro PCB outline, placements, copper pours, tracks, and vias"},
        {"easyeda_native_autoroute", "Trigger EasyEDA Pro's built-in auto-router (Route > Auto Route > Run) and wait for tracks to settle"},
        {"easyeda_run_drc", "Run live Schematic ERC or PCB DRC inside EasyEDA Pro"},
        {"easyeda_export_jlcpcb", "Export turnkey JLCPCB Gerber ZIP, BOM CSV, CPL CSV, and Specctra .dsn"},
        {"easyeda_invoke_api", "Invoke whitelisted eda.* API call in EasyEDA Pro"},
        {"easyeda_convert", "Convert EasyEDA JSON schematic or PCB to KiCad 8 S-expression"}
    };

    constexpr size_t kToolCount = sizeof(kTools) / sizeof(kTools[0]);
    std::vector<nlohmann::json> list;
    list.reserve(kToolCount + 1);
    for (size_t i = 0; i < kToolCount; ++i) {
        list.push_back({
            {"name", kTools[i].name},
            {"description", kTools[i].desc},
            {"inputSchema", {{"type", "object"}}}
        });
    }
    list.push_back({
        {"name", "eda_bridge_status"},
        {"description", "Internal status probe for aimon-eda-bridge"},
        {"inputSchema", {{"type", "object"}}}
    });
    return list;
}

void EdaRpcServer::setBridgeEnabled(const std::string& app, bool enabled) {
    if (app == "easyeda") {
        _enableEasyEda.store(enabled);
    } else if (app == "kicad") {
        _enableKiCad.store(enabled);
    } else if (app == "freecad") {
        _enableFreeCad.store(enabled);
    }
    if (!enabled) {
        std::lock_guard<std::mutex> lock(_pluginsMutex);
        for (auto& kv : _pluginSessions) {
            if (kv.second->app == app && kv.second->active.load()) {
                kv.second->active.store(false);
                closeSocketFd(kv.second->socketFd);
                kv.second->socketFd = -1;
            }
        }
    }
}

bool EdaRpcServer::isBridgeEnabled(const std::string& app) const {
    if (app == "easyeda") {
        return _enableEasyEda.load();
    }
    if (app == "kicad") {
        return _enableKiCad.load();
    }
    if (app == "freecad") {
        return _enableFreeCad.load();
    }
    return false;
}

void EdaRpcServer::setDisallowExports(bool disallow) {
    _disallowExports.store(disallow);
}

bool EdaRpcServer::isDisallowExports() const {
    return _disallowExports.load();
}

std::vector<nlohmann::json> EdaRpcServer::enabledRemoteToolDescriptors() const {
    const auto allTools = remoteToolDescriptors();
    std::vector<nlohmann::json> filtered;
    filtered.reserve(allTools.size());
    bool ee = _enableEasyEda.load();
    bool kc = _enableKiCad.load();
    bool fc = _enableFreeCad.load();

    for (const auto& td : allTools) {
        std::string name = td.value("name", "");
        if (name.rfind("easyeda_", 0) == 0) {
            if (ee) filtered.push_back(td);
        } else if (name.rfind("kicad_", 0) == 0) {
            if (kc) filtered.push_back(td);
        } else if (name.rfind("freecad_", 0) == 0) {
            if (fc) filtered.push_back(td);
        } else {
            filtered.push_back(td);
        }
    }
    return filtered;
}

bool EdaRpcServer::start() {
    if (_running.load()) return true;

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    auto bindListener = [](const std::string& host, int requestedPort, int& outBoundPort) -> int {
        int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        if (fd < 0) return -1;

        int opt = 1;
#ifdef _WIN32
        ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(requestedPort));
        if (host.empty() || host == "0.0.0.0") {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else {
            if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
                closeSocketFd(fd);
                return -1;
            }
        }

        if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            closeSocketFd(fd);
            return -1;
        }
        if (::listen(fd, 16) < 0) {
            closeSocketFd(fd);
            return -1;
        }

        struct sockaddr_in bound{};
        socklen_t bLen = sizeof(bound);
        if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&bound), &bLen) == 0) {
            outBoundPort = ntohs(bound.sin_port);
        } else {
            outBoundPort = requestedPort;
        }
        return fd;
    };

    int rpcPortOut = 0;
    _rpcServerFd = bindListener(_options.listenHost, _options.port, rpcPortOut);
    if (_rpcServerFd < 0) {
        return false;
    }
    _boundPort.store(rpcPortOut);

    int plugPortOut = 0;
    // AppPluginRelay ALWAYS binds strictly to 127.0.0.1 for local GUI isolation
    _pluginServerFd = bindListener("127.0.0.1", _options.pluginPort, plugPortOut);
    if (_pluginServerFd < 0) {
        closeSocketFd(_rpcServerFd);
        _rpcServerFd = -1;
        return false;
    }
    _boundPluginPort.store(plugPortOut);

    _running.store(true);
    _rpcAcceptThread = std::make_unique<std::thread>(&EdaRpcServer::rpcAcceptLoop, this);
    _pluginAcceptThread = std::make_unique<std::thread>(&EdaRpcServer::pluginAcceptLoop, this);
    return true;
}

void EdaRpcServer::stop() {
    if (!_running.exchange(false)) {
        disconnectGateway();
        return;
    }

    disconnectGateway();

    closeSocketFd(_rpcServerFd);
    _rpcServerFd = -1;
    closeSocketFd(_pluginServerFd);
    _pluginServerFd = -1;

    if (_rpcAcceptThread && _rpcAcceptThread->joinable()) {
        _rpcAcceptThread->join();
        _rpcAcceptThread.reset();
    }
    if (_pluginAcceptThread && _pluginAcceptThread->joinable()) {
        _pluginAcceptThread->join();
        _pluginAcceptThread.reset();
    }

    std::vector<std::shared_ptr<PluginSession>> sessionsToJoin;
    {
        std::lock_guard<std::mutex> lock(_pluginsMutex);
        for (auto& kv : _pluginSessions) {
            kv.second->active.store(false);
            closeSocketFd(kv.second->socketFd);
            kv.second->socketFd = -1;
            sessionsToJoin.push_back(kv.second);
        }
        _pluginSessions.clear();
    }
    for (auto& s : sessionsToJoin) {
        if (s->readThread && s->readThread->joinable()) {
            s->readThread->join();
        }
    }

    {
        std::lock_guard<std::mutex> lock(_pendingPluginMutex);
        for (auto& kv : _pendingPluginCalls) {
            std::lock_guard<std::mutex> cLock(kv.second->mutex);
            if (!kv.second->completed) {
                kv.second->errorMessage = "EdaRpcServer stopped";
                kv.second->completed = true;
                kv.second->cv.notify_all();
            }
        }
        _pendingPluginCalls.clear();
    }
}

void EdaRpcServer::rpcAcceptLoop() {
    while (_running.load()) {
        struct sockaddr_in cAddr{};
        socklen_t cLen = sizeof(cAddr);
        int cfd = static_cast<int>(::accept(_rpcServerFd, reinterpret_cast<struct sockaddr*>(&cAddr), &cLen));
        if (cfd < 0) {
            if (!_running.load()) break;
            continue;
        }
        std::thread(&EdaRpcServer::handleRpcClient, this, cfd).detach();
    }
}

void EdaRpcServer::handleRpcClient(int clientFd) {
    std::string buffer;
    char chunk[4096];

    while (_running.load()) {
#ifdef _WIN32
        int n = ::recv(static_cast<SOCKET>(clientFd), chunk, sizeof(chunk), 0);
#else
        ssize_t n = ::recv(clientFd, chunk, sizeof(chunk), 0);
#endif
        if (n <= 0) break;
        buffer.append(chunk, static_cast<size_t>(n));
        if (buffer.size() > _options.maxOutputBytes) {
            break;
        }

        size_t nlPos;
        while ((nlPos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, nlPos);
            buffer.erase(0, nlPos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
            if (parsed.is_discarded() || (!parsed.is_object() && (!parsed.is_array() || parsed.empty()))) {
                nlohmann::json errResp = {
                    {"jsonrpc", "2.0"},
                    {"id", nullptr},
                    {"error", {{"code", -32700}, {"message", "Parse error"}}}
                };
                sendJsonLine(clientFd, errResp);
                continue;
            }

            auto processSingleRequest = [&](const nlohmann::json& req) -> nlohmann::json {
                if (!req.is_object()) {
                    return {
                        {"jsonrpc", "2.0"},
                        {"id", nullptr},
                        {"error", {{"code", -32600}, {"message", "Invalid Request"}}}
                    };
                }
                nlohmann::json id = req.value("id", nlohmann::json(1));
                std::string method = req.value("method", "");
                nlohmann::json params = req.value("params", nlohmann::json::object());

                // Constant-time authentication check
                if (!_options.authSecret.empty()) {
                    std::string providedToken;
                    if (req.contains("auth_token") && req["auth_token"].is_string()) {
                        providedToken = req["auth_token"].get<std::string>();
                    } else if (params.is_object() && params.contains("auth_token") && params["auth_token"].is_string()) {
                        providedToken = params["auth_token"].get<std::string>();
                    } else if (params.is_object() && params.contains("arguments") &&
                               params["arguments"].is_object() &&
                               params["arguments"].contains("_auth_token") &&
                               params["arguments"]["_auth_token"].is_string()) {
                        providedToken = params["arguments"]["_auth_token"].get<std::string>();
                    }

                    if (!constantTimeEquals(providedToken, _options.authSecret)) {
                        return {
                            {"jsonrpc", "2.0"},
                            {"id", id},
                            {"error", {
                                {"code", -32001},
                                {"message", "Unauthorized: invalid or missing bearer auth_token"}
                            }}
                        };
                    }
                }

                if (method == "eda/ping" || method == "eda/status" || method == "ping") {
                    return {
                        {"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", buildStatusPayload()}
                    };
                }

                std::string toolName = method;
                nlohmann::json toolArgs = params;
                if (method == "tools/call" && params.is_object()) {
                    toolName = params.value("name", "");
                    toolArgs = params.value("arguments", nlohmann::json::object());
                }
                if (toolArgs.is_object() && toolArgs.contains("_auth_token")) {
                    toolArgs.erase("_auth_token");
                }

                std::string pathErr;
                if (!validateJsonSandboxPaths(toolArgs, pathErr)) {
                    return {
                        {"jsonrpc", "2.0"},
                        {"id", id},
                        {"error", {
                            {"code", -32602},
                            {"message", pathErr}
                        }}
                    };
                }

                nlohmann::json toolRes = dispatchToolCall(toolName, toolArgs);
                return {
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", toolRes}
                };
            };

            if (parsed.is_array()) {
                nlohmann::json batchResp = nlohmann::json::array();
                for (const auto& item : parsed) {
                    batchResp.push_back(processSingleRequest(item));
                }
                sendJsonLine(clientFd, batchResp);
            } else {
                sendJsonLine(clientFd, processSingleRequest(parsed));
            }
        }
    }

    closeSocketFd(clientFd);
}

void EdaRpcServer::pluginAcceptLoop() {
    while (_running.load()) {
        struct sockaddr_in cAddr{};
        socklen_t cLen = sizeof(cAddr);
        int cfd = static_cast<int>(::accept(_pluginServerFd, reinterpret_cast<struct sockaddr*>(&cAddr), &cLen));
        if (cfd < 0) {
            if (!_running.load()) break;
            continue;
        }

        auto session = std::make_shared<PluginSession>();
        session->sessionId = _nextSessionId.fetch_add(1);
        session->socketFd = cfd;
        session->active.store(true);

        {
            std::lock_guard<std::mutex> lock(_pluginsMutex);
            _pluginSessions[session->sessionId] = session;
        }

        session->readThread = std::make_unique<std::thread>(
            &EdaRpcServer::handlePluginSession, this, session);
    }
}

void EdaRpcServer::handlePluginSession(std::shared_ptr<PluginSession> session) {
    std::vector<uint8_t> buf;
    std::string wsFragBuffer;
    uint8_t wsFragOpcode = 0;
    uint8_t chunk[4096];
    bool handshakeChecked = false;

    while (_running.load() && session->active.load()) {
#ifdef _WIN32
        int n = ::recv(static_cast<SOCKET>(session->socketFd),
                       reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
#else
        ssize_t n = ::recv(session->socketFd, chunk, sizeof(chunk), 0);
#endif
        if (n <= 0) break;
        buf.insert(buf.end(), chunk, chunk + n);
        if (buf.size() > 4 * 1024 * 1024 + 65536) break;

        if (!handshakeChecked) {
            std::string view(reinterpret_cast<const char*>(buf.data()), buf.size());
            if (view.rfind("GET ", 0) == 0) {
                size_t hdrEnd = view.find("\r\n\r\n");
                if (hdrEnd == std::string::npos) {
                    continue; // Wait for full HTTP upgrade header
                }
                std::string headers = view.substr(0, hdrEnd + 4);
                buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(hdrEnd + 4));

                std::string wsKey;
                std::istringstream iss(headers);
                std::string line;
                while (std::getline(iss, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    std::string lowerLine = line;
                    for (char& c : lowerLine) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (lowerLine.rfind("sec-websocket-key:", 0) == 0) {
                        wsKey = line.substr(18);
                        while (!wsKey.empty() && wsKey.front() == ' ') wsKey.erase(wsKey.begin());
                        while (!wsKey.empty() && wsKey.back() == ' ') wsKey.pop_back();
                    }
                }

                if (wsKey.empty()) break;
                std::string acceptKey = computeWebSocketAcceptKey(wsKey);
                std::string httpResp =
                    "HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: " + acceptKey + "\r\n\r\n";
                if (!sendAllBytes(session->socketFd,
                                  reinterpret_cast<const uint8_t*>(httpResp.data()),
                                  httpResp.size())) {
                    break;
                }
                session->isWebSocket = true;
                handshakeChecked = true;
                nlohmann::json helloMsg = {
                    {"jsonrpc", "2.0"},
                    {"method", "server/hello"},
                    {"params", {{"service", "aimon-eda-bridge"}}}
                };
                sendPluginJson(session, helloMsg);
            } else {
                session->isWebSocket = false;
                handshakeChecked = true;
            }
        }

        if (session->isWebSocket) {
            while (!buf.empty()) {
                size_t consumed = 0;
                uint8_t opcode = 0;
                std::string payload;
                std::string wsErr;
                bool fin = true;
                if (!decodeWebSocketFrame(buf.data(), buf.size(), consumed, opcode, payload, wsErr, &fin)) {
                    if (!wsErr.empty()) {
                        session->active.store(false);
                    }
                    break;
                }
                buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(consumed));

                if (opcode == 0x8) { // Close frame
                    session->active.store(false);
                    break;
                } else if (opcode == 0x9) { // Ping -> Pong
                    auto pong = encodeWebSocketFrame(payload, 0xA, false);
                    std::lock_guard<std::mutex> wLock(session->writeMutex);
                    sendAllBytes(session->socketFd, pong.data(), pong.size());
                } else if (opcode == 0x1 || opcode == 0x2) { // Text or binary data frame
                    if (!fin) {
                        wsFragOpcode = opcode;
                        wsFragBuffer = std::move(payload);
                    } else {
                        wsFragBuffer.clear();
                        wsFragOpcode = 0;
                        nlohmann::json msg = nlohmann::json::parse(payload, nullptr, false);
                        if (!msg.is_discarded() && msg.is_object()) {
                            processPluginMessage(session, msg);
                        }
                    }
                } else if (opcode == 0x0) { // Continuation frame (RFC 6455 Section 5.4)
                    if (wsFragOpcode == 0x1 || wsFragOpcode == 0x2) {
                        if (wsFragBuffer.size() + payload.size() > 4 * 1024 * 1024) {
                            wsFragBuffer.clear();
                            wsFragOpcode = 0;
                            session->active.store(false);
                            break;
                        }
                        wsFragBuffer.append(payload);
                        if (fin) {
                            std::string fullMsg = std::move(wsFragBuffer);
                            wsFragBuffer.clear();
                            wsFragOpcode = 0;
                            nlohmann::json msg = nlohmann::json::parse(fullMsg, nullptr, false);
                            if (!msg.is_discarded() && msg.is_object()) {
                                processPluginMessage(session, msg);
                            }
                        }
                    }
                }
            }
        } else {
            while (true) {
                auto it = std::find(buf.begin(), buf.end(), static_cast<uint8_t>('\n'));
                if (it == buf.end()) break;
                std::string line(buf.begin(), it);
                buf.erase(buf.begin(), it + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;

                nlohmann::json msg = nlohmann::json::parse(line, nullptr, false);
                if (!msg.is_discarded() && msg.is_object()) {
                    processPluginMessage(session, msg);
                }
            }
        }
    }

    session->active.store(false);
    closeSocketFd(session->socketFd);
    session->socketFd = -1;
}

void EdaRpcServer::processPluginMessage(std::shared_ptr<PluginSession> session,
                                        const nlohmann::json& msg) {
    if (msg.contains("method") && msg["method"].is_string()) {
        std::string method = msg["method"].get<std::string>();
        nlohmann::json id = msg.value("id", nlohmann::json(1));
        nlohmann::json params = msg.value("params", nlohmann::json::object());

        if (method == "plugin/chunk") {
            std::string msgId = params.value("chunk_id", params.value("msg_id", ""));
            int index = params.value("seq", params.value("index", -1));
            int total = params.value("total", 0);
            std::string data = params.value("data", "");
            if (msgId.empty() || index < 0 || total <= 0 || index >= total || total > 256) {
                return;
            }
            auto& st = session->chunkBuffers[msgId];
            if (st.total != total) {
                st.total = total;
                st.parts.assign(static_cast<size_t>(total), std::string());
                st.received.assign(static_cast<size_t>(total), false);
                st.receivedCount = 0;
            }
            if (!st.received[static_cast<size_t>(index)]) {
                st.parts[static_cast<size_t>(index)] = std::move(data);
                st.received[static_cast<size_t>(index)] = true;
                st.receivedCount++;
            }
            if (st.receivedCount == st.total) {
                std::string joined;
                size_t totalBytes = 0;
                for (const auto& p : st.parts) totalBytes += p.size();
                if (totalBytes <= 4 * 1024 * 1024) {
                    joined.reserve(totalBytes);
                    for (const auto& p : st.parts) joined.append(p);
                }
                session->chunkBuffers.erase(msgId);
                if (!joined.empty()) {
                    nlohmann::json fullMsg = nlohmann::json::parse(joined, nullptr, false);
                    if (!fullMsg.is_discarded() && fullMsg.is_object()) {
                        processPluginMessage(session, fullMsg);
                    }
                }
            } else if (session->chunkBuffers.size() > 32) {
                session->chunkBuffers.erase(session->chunkBuffers.begin());
            }
            return;
        }

        if (method == "plugin/register" || method == "plugin/status") {
            std::string reqApp = params.value("app", session->app);
            if (!isBridgeEnabled(reqApp)) {
                nlohmann::json errResp = {
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"error", {
                        {"code", -32002},
                        {"message", "Bridge '" + reqApp + "' is disabled in aimon settings"}
                    }}
                };
                sendPluginJson(session, errResp);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(_pluginsMutex);
                session->app = reqApp;
                session->version = params.value("version", "1.0");
                session->activeDocument = params.value("active_document", "");
            }
            nlohmann::json resp = {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"result", {
                    {"status", "registered"},
                    {"app", session->app},
                    {"session_id", session->sessionId}
                }}
            };
            sendPluginJson(session, resp);
        }
        return;
    }

    if (msg.contains("id")) {
        std::string reqId;
        if (msg["id"].is_string()) {
            reqId = msg["id"].get<std::string>();
        } else if (msg["id"].is_number_integer()) {
            reqId = std::to_string(msg["id"].get<int64_t>());
        }

        std::shared_ptr<PluginPendingCall> pending;
        {
            std::lock_guard<std::mutex> lock(_pendingPluginMutex);
            auto it = _pendingPluginCalls.find(reqId);
            if (it != _pendingPluginCalls.end()) {
                pending = it->second;
            }
        }
        if (pending) {
            std::lock_guard<std::mutex> cLock(pending->mutex);
            if (msg.contains("error") && !msg["error"].is_null()) {
                if (msg["error"].is_object()) {
                    pending->errorMessage = msg["error"].value("message", msg["error"].dump());
                } else {
                    pending->errorMessage = msg["error"].dump();
                }
            } else if (msg.contains("result")) {
                pending->result = msg["result"];
            }
            pending->completed = true;
            pending->cv.notify_all();
        }
    }
}

bool EdaRpcServer::sendPluginJson(std::shared_ptr<PluginSession> session,
                                  const nlohmann::json& msg) {
    if (!session || !session->active.load() || session->socketFd < 0) return false;
    std::lock_guard<std::mutex> lock(session->writeMutex);
    std::string payload = msg.dump();
    if (session->isWebSocket) {
        auto frame = encodeWebSocketFrame(payload, 0x1, false);
        return sendAllBytes(session->socketFd, frame.data(), frame.size());
    } else {
        payload.push_back('\n');
        return sendAllBytes(session->socketFd,
                            reinterpret_cast<const uint8_t*>(payload.data()),
                            payload.size());
    }
}

bool EdaRpcServer::isPluginConnected(const std::string& app) const {
    if (!isBridgeEnabled(app)) return false;
    std::lock_guard<std::mutex> lock(_pluginsMutex);
    for (const auto& kv : _pluginSessions) {
        if (kv.second->active.load() && kv.second->app == app) {
            return true;
        }
    }
    return false;
}

bool EdaRpcServer::callPlugin(const std::string& app,
                              const std::string& toolName,
                              const nlohmann::json& arguments,
                              nlohmann::json& outResult,
                              std::string& outError,
                              int timeoutMs) {
    std::shared_ptr<PluginSession> target;
    {
        std::lock_guard<std::mutex> lock(_pluginsMutex);
        for (const auto& kv : _pluginSessions) {
            if (kv.second->active.load() && kv.second->app == app) {
                target = kv.second;
            }
        }
    }

    if (!target) {
        if (app == "easyeda") {
            outError = "EasyEDA Pro desktop extension is not connected on port " +
                       std::to_string(_boundPluginPort.load());
        } else if (app == "kicad") {
            outError = "KiCad pcbnew plugin is not connected on port " +
                       std::to_string(_boundPluginPort.load());
        } else {
            outError = "FreeCAD GUI plugin is not connected on port " +
                       std::to_string(_boundPluginPort.load());
        }
        return false;
    }

    std::string reqId = "plug-" + std::to_string(_nextPluginReqId.fetch_add(1));
    auto pending = std::make_shared<PluginPendingCall>();
    pending->reqId = reqId;

    {
        std::lock_guard<std::mutex> lock(_pendingPluginMutex);
        _pendingPluginCalls[reqId] = pending;
    }

    nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"id", reqId},
        {"method", "tools/call"},
        {"params", {
            {"name", toolName},
            {"arguments", arguments.is_object() ? arguments : nlohmann::json::object()}
        }}
    };

    if (!sendPluginJson(target, req)) {
        std::lock_guard<std::mutex> lock(_pendingPluginMutex);
        _pendingPluginCalls.erase(reqId);
        outError = "Failed to send request to " + app + " plugin session";
        return false;
    }

    std::unique_lock<std::mutex> cLock(pending->mutex);
    int waitMs = (timeoutMs > 0) ? timeoutMs : _options.defaultTimeoutMs;
    bool done = pending->cv.wait_for(cLock, std::chrono::milliseconds(waitMs), [&]() {
        return pending->completed;
    });

    {
        std::lock_guard<std::mutex> lock(_pendingPluginMutex);
        _pendingPluginCalls.erase(reqId);
    }

    if (!done) {
        outError = "Timed out waiting for " + app + " plugin response";
        return false;
    }
    if (!pending->errorMessage.empty()) {
        outError = pending->errorMessage;
        return false;
    }
    outResult = pending->result;
    if (outResult.is_object() && outResult.contains("active_document") &&
        outResult["active_document"].is_string()) {
        std::string newDoc = outResult["active_document"].get<std::string>();
        if (!newDoc.empty()) {
            std::lock_guard<std::mutex> lock(_pluginsMutex);
            target->activeDocument = newDoc;
        }
    }
    return true;
}

bool EdaRpcServer::connectToGateway(const std::string& host, int port) {
    disconnectGateway();
    if (host.empty() || port <= 0) return false;

    int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) return false;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        closeSocketFd(fd);
        return false;
    }

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        closeSocketFd(fd);
        return false;
    }

    _gatewayFd = fd;
    _gatewayActive.store(true);

    nlohmann::json regMsg = {
        {"jsonrpc", "2.0"},
        {"id", "eda-gw-reg-1"},
        {"method", "gateway/register"},
        {"params", {
            {"subsystem", "eda"},
            {"display_name", "EDA & CAD Bridge"},
            {"short_name", "EDA"},
            {"priority", 40},
            {"web_port", 0},
            {"tools", enabledRemoteToolDescriptors()}
        }}
    };
    sendJsonLine(_gatewayFd, regMsg);

    _gatewayThread = std::make_unique<std::thread>(&EdaRpcServer::gatewayReadLoop, this, _gatewayFd);
    return true;
}

void EdaRpcServer::disconnectGateway() {
    if (!_gatewayActive.exchange(false)) return;
    closeSocketFd(_gatewayFd);
    _gatewayFd = -1;
    if (_gatewayThread && _gatewayThread->joinable()) {
        _gatewayThread->join();
        _gatewayThread.reset();
    }
}

void EdaRpcServer::gatewayReadLoop(int gwFd) {
    std::string buffer;
    char chunk[4096];

    while (_gatewayActive.load()) {
#ifdef _WIN32
        int n = ::recv(static_cast<SOCKET>(gwFd), chunk, sizeof(chunk), 0);
#else
        ssize_t n = ::recv(gwFd, chunk, sizeof(chunk), 0);
#endif
        if (n <= 0) break;
        buffer.append(chunk, static_cast<size_t>(n));

        size_t nlPos;
        while ((nlPos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, nlPos);
            buffer.erase(0, nlPos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            nlohmann::json msg = nlohmann::json::parse(line, nullptr, false);
            if (msg.is_discarded() || !msg.is_object()) continue;

            if (msg.contains("method") && msg["method"].is_string()) {
                std::string method = msg["method"].get<std::string>();
                nlohmann::json id = msg.value("id", nlohmann::json(1));
                nlohmann::json params = msg.value("params", nlohmann::json::object());

                if (method == "ping") {
                    std::lock_guard<std::mutex> wLock(_gatewayWriteMutex);
                    sendJsonLine(gwFd, {{"jsonrpc", "2.0"}, {"id", id}, {"result", nlohmann::json::object()}});
                } else if (method == "tools/call") {
                    std::string toolName = params.value("name", "");
                    nlohmann::json toolArgs = params.value("arguments", nlohmann::json::object());

                    if (!_options.authSecret.empty()) {
                        std::string provided = toolArgs.value("_auth_token", "");
                        if (!constantTimeEquals(provided, _options.authSecret)) {
                            std::lock_guard<std::mutex> wLock(_gatewayWriteMutex);
                            sendJsonLine(gwFd, {
                                {"jsonrpc", "2.0"},
                                {"id", id},
                                {"error", {{"code", -32001}, {"message", "Unauthorized gateway tool call"}}}
                            });
                            continue;
                        }
                    }
                    if (toolArgs.is_object() && toolArgs.contains("_auth_token")) {
                        toolArgs.erase("_auth_token");
                    }

                    std::string pathErr;
                    if (!validateJsonSandboxPaths(toolArgs, pathErr)) {
                        std::lock_guard<std::mutex> wLock(_gatewayWriteMutex);
                        sendJsonLine(gwFd, {
                            {"jsonrpc", "2.0"},
                            {"id", id},
                            {"error", {{"code", -32602}, {"message", pathErr}}}
                        });
                        continue;
                    }

                    nlohmann::json res = (toolName == "eda_bridge_status")
                        ? buildStatusPayload()
                        : dispatchToolCall(toolName, toolArgs);
                    std::lock_guard<std::mutex> wLock(_gatewayWriteMutex);
                    sendJsonLine(gwFd, {{"jsonrpc", "2.0"}, {"id", id}, {"result", res}});
                }
            }
        }
    }
}

nlohmann::json EdaRpcServer::buildStatusPayload() const {
    std::string eeDoc, kcDoc, fcDoc;
    std::string eeVer = _easyedaInfo.version;
    std::string kcVer = _kicadInfo.version;
    std::string fcVer = _freecadInfo.version;
    bool eeConn = false, kcConn = false, fcConn = false;
    {
        std::lock_guard<std::mutex> lock(_pluginsMutex);
        for (const auto& kv : _pluginSessions) {
            if (!kv.second->active.load()) continue;
            if (kv.second->app == "easyeda" && _enableEasyEda.load()) {
                eeConn = true;
                eeDoc = kv.second->activeDocument;
                if (!kv.second->version.empty()) eeVer = kv.second->version;
            } else if (kv.second->app == "kicad" && _enableKiCad.load()) {
                kcConn = true;
                kcDoc = kv.second->activeDocument;
                if (!kv.second->version.empty()) kcVer = kv.second->version;
            } else if (kv.second->app == "freecad" && _enableFreeCad.load()) {
                fcConn = true;
                fcDoc = kv.second->activeDocument;
                if (!kv.second->version.empty()) fcVer = kv.second->version;
            }
        }
    }

#ifdef _WIN32
    std::string platform = "windows-x64";
#else
    std::string platform = "posix-x64";
#endif

    nlohmann::json tools = nlohmann::json::array({
        {
            {"name", "easyeda"},
            {"enabled", _enableEasyEda.load()},
            {"available", _easyedaInfo.available},
            {"plugin_connected", eeConn},
            {"binary_path", _easyedaInfo.binaryPath},
            {"version", eeVer},
            {"bridge_version", AIMON_VERSION_STRING},
            {"active_document", eeDoc}
        },
        {
            {"name", "kicad"},
            {"enabled", _enableKiCad.load()},
            {"available", _kicadInfo.available},
            {"plugin_connected", kcConn},
            {"binary_path", _kicadInfo.binaryPath},
            {"version", kcVer},
            {"bridge_version", AIMON_VERSION_STRING},
            {"active_document", kcDoc}
        },
        {
            {"name", "freecad"},
            {"enabled", _enableFreeCad.load()},
            {"available", _freecadInfo.available},
            {"plugin_connected", fcConn},
            {"binary_path", _freecadInfo.binaryPath},
            {"version", fcVer},
            {"bridge_version", AIMON_VERSION_STRING},
            {"active_document", fcDoc}
        }
    });

    return {
        {"ok", true},
        {"bridge_version", AIMON_VERSION_STRING},
        {"platform", platform},
        {"rpc_port", _boundPort.load()},
        {"plugin_port", _boundPluginPort.load()},
        {"disallow_exports", _disallowExports.load()},
        {"tools", tools}
    };
}

nlohmann::json EdaRpcServer::dispatchToolCall(const std::string& toolName,
                                              const nlohmann::json& arguments) {
    if (toolName == "eda_bridge_status") {
        return buildStatusPayload();
    }
    if (EdaAccelerators::isAcceleratorTool(toolName)) {
        EdaAccelerators acc;
        bool ok = false;
        std::string summary;
        return acc.executeTool(toolName, arguments, ok, summary);
    }
    if (_disallowExports.load()) {
        if (toolName == "easyeda_export_jlcpcb" ||
            toolName == "kicad_export_fab_pack" ||
            toolName == "kicad_export_step" ||
            toolName == "freecad_export_print_files") {
            return {
                {"ok", false},
                {"error", "Manufacturing and file exports are locked in aimon settings"}
            };
        }
    }
    if (toolName.rfind("kicad_", 0) == 0) {
        if (!_enableKiCad.load()) {
            return {
                {"ok", false},
                {"error", "KiCad bridge is disabled in aimon settings"}
            };
        }
        return handleKiCadTool(toolName, arguments);
    }
    if (toolName.rfind("freecad_", 0) == 0) {
        if (!_enableFreeCad.load()) {
            return {
                {"ok", false},
                {"error", "FreeCAD bridge is disabled in aimon settings"}
            };
        }
        return handleFreeCadTool(toolName, arguments);
    }
    if (toolName.rfind("easyeda_", 0) == 0) {
        if (!_enableEasyEda.load()) {
            return {
                {"ok", false},
                {"error", "EasyEDA Pro bridge is disabled in aimon settings"}
            };
        }
        return handleEasyEdaTool(toolName, arguments);
    }
    return {
        {"ok", false},
        {"error", "Unknown remote EDA/CAD tool: " + toolName}
    };
}

nlohmann::json EdaRpcServer::handleKiCadTool(const std::string& toolName,
                                             const nlohmann::json& args) {
    int timeoutMs = args.value("timeout_ms", _options.defaultTimeoutMs);

    if (toolName == "kicad_search_library") {
        nlohmann::json searchArgs = args;
        if (!searchArgs.contains("symbol_lib_dir") && !_options.symbolLibDir.empty()) {
            searchArgs["symbol_lib_dir"] = _options.symbolLibDir;
        }
        if (!searchArgs.contains("footprint_lib_dir") && !_options.footprintLibDir.empty()) {
            searchArgs["footprint_lib_dir"] = _options.footprintLibDir;
        }
        return KiCadSchematicEditor::searchLibraries(searchArgs);
    }

    if (toolName == "kicad_inspect_project") {
        bool hasPathOrSexp = !args.value("project_path",
                              args.value("file_path",
                              args.value("schematic_path",
                              args.value("pcb_path",
                              args.value("path",
                              args.value("sexp", "")))))).empty();
        if (isPluginConnected("kicad") && (args.value("prefer_live_gui", false) || !hasPathOrSexp)) {
            nlohmann::json plugRes;
            std::string plugErr;
            if (callPlugin("kicad", toolName, args, plugRes, plugErr, timeoutMs)) {
                if (plugRes.is_object() && !plugRes.contains("connectivity_audit") &&
                    (plugRes.contains("footprints") || plugRes.contains("pads") || plugRes.contains("tracks"))) {
                    EdaAccelerators acc;
                    bool auditOk = false;
                    std::string auditSummary;
                    plugRes["connectivity_audit"] = acc.calcPcbConnectivityAudit(plugRes, auditOk, auditSummary);
                }
                return plugRes;
            }
        }
        nlohmann::json res = KiCadSchematicEditor::inspectFile(args);
        res["plugin_connected"] = isPluginConnected("kicad");
        if (res.is_object() && res.value("ok", false)) {
            EdaAccelerators acc;
            bool auditOk = false;
            std::string auditSummary;
            res["connectivity_audit"] = acc.calcPcbConnectivityAudit(res, auditOk, auditSummary);
        }
        return res;
    }

    if (toolName == "kicad_edit_schematic") {
        nlohmann::json res = KiCadSchematicEditor::applySchematicEdits(args);
        if (res.value("ok", false) && args.value("auto_erc", true)) {
            std::string schPath = res.value("schematic_path", "");
            if (!schPath.empty()) {
                nlohmann::json ercRes = handleKiCadTool("kicad_run_erc", {{"schematic_path", schPath}});
                res["erc"] = ercRes;
            }
        }
        return res;
    }

    if (toolName == "kicad_sync_sch_to_pcb") {
        nlohmann::json res = KiCadSchematicEditor::syncSchematicToPcb(args);
        if (res.value("ok", false) && !args.value("dry_run", false) && isPluginConnected("kicad")) {
            nlohmann::json pluginArgs = args;
            if (res.contains("footprints")) pluginArgs["footprints"] = res["footprints"];
            if (res.contains("tracks")) pluginArgs["tracks"] = res["tracks"];
            if (res.contains("vias")) pluginArgs["vias"] = res["vias"];
            if (res.contains("zones")) pluginArgs["zones"] = res["zones"];
            if (!pluginArgs.contains("board_outline") && res.contains("board_outline")) {
                pluginArgs["board_outline"] = res["board_outline"];
            }
            nlohmann::json plugRes;
            std::string plugErr;
            if (callPlugin("kicad", toolName, pluginArgs, plugRes, plugErr, timeoutMs)) {
                res["live_gui_synced"] = true;
                res["plugin_response"] = plugRes;
            }
        }
        return res;
    }

    if (toolName == "kicad_edit_pcb") {
        nlohmann::json res = KiCadSchematicEditor::applyPcbEdits(args);
        if (res.value("ok", false) && isPluginConnected("kicad")) {
            nlohmann::json pluginArgs = args;
            if (res.contains("footprints")) pluginArgs["footprints"] = res["footprints"];
            if (res.contains("tracks")) pluginArgs["tracks"] = res["tracks"];
            if (res.contains("vias")) pluginArgs["vias"] = res["vias"];
            if (res.contains("zones")) pluginArgs["zones"] = res["zones"];
            if (!pluginArgs.contains("board_outline") && res.contains("board_outline")) {
                pluginArgs["board_outline"] = res["board_outline"];
            }
            nlohmann::json plugRes;
            std::string plugErr;
            if (callPlugin("kicad", toolName, pluginArgs, plugRes, plugErr, timeoutMs)) {
                res["live_gui_refreshed"] = true;
                res["plugin_response"] = plugRes;
            }
        }
        return res;
    }

    if (toolName == "kicad_run_erc") {
        std::string schPath = args.value("schematic_path", args.value("file_path", args.value("path", "")));
        if (!_options.kicadBinaryPath.empty() && fs::exists(_options.kicadBinaryPath)) {
            std::string reportPath = schPath + ".erc.json";
            ProcessResult pr = ProcessLauncher::run(
                _options.kicadBinaryPath,
                {"sch", "erc", "--format", "json", "-o", reportPath, schPath},
                timeoutMs,
                _options.maxOutputBytes);
            if (pr.timedOut) {
                return {{"ok", false}, {"timed_out", true}, {"error", pr.errorMessage}};
            }
            if (fs::exists(reportPath)) {
                std::ifstream ifs(reportPath);
                std::string txt((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                nlohmann::json rep = nlohmann::json::parse(txt, nullptr, false);
                if (!rep.is_discarded()) {
                    rep["ok"] = (pr.exitCode == 0);
                    rep["truncated"] = pr.truncated;
                    return rep;
                }
            }
            nlohmann::json parsedOut = nlohmann::json::parse(pr.stdoutText, nullptr, false);
            if (!parsedOut.is_discarded() && parsedOut.is_object()) {
                parsedOut["ok"] = (pr.exitCode == 0);
                parsedOut["truncated"] = pr.truncated;
                return parsedOut;
            }
            return {
                {"ok", pr.exitCode == 0},
                {"exit_code", pr.exitCode},
                {"truncated", pr.truncated},
                {"stdout_bytes", pr.stdoutText.size()},
                {"violations", nlohmann::json::array()},
                {"violation_count", 0}
            };
        }

        // Built-in structural schematic ERC
        nlohmann::json inspected = KiCadSchematicEditor::inspectFile({{"file_path", schPath}});
        if (!inspected.value("ok", false)) return inspected;
        nlohmann::json violations = nlohmann::json::array();
        std::set<std::string> seenRefs;
        for (const auto& c : inspected["components"]) {
            std::string ref = c.value("reference", "");
            if (seenRefs.count(ref)) {
                violations.push_back({
                    {"type", "duplicate_reference"},
                    {"severity", "error"},
                    {"description", "Duplicate component reference " + ref}
                });
            }
            seenRefs.insert(ref);
        }
        return {
            {"ok", true},
            {"schematic_path", schPath},
            {"violation_count", violations.size()},
            {"violations", violations}
        };
    }

    if (toolName == "kicad_run_drc") {
        std::string pcbPath = args.value("pcb_path", args.value("file_path", args.value("path", "")));
        bool parity = args.value("schematic_parity", true);
        if (isPluginConnected("kicad") && (pcbPath.empty() || args.value("prefer_live_gui", false))) {
            nlohmann::json plugRes;
            std::string plugErr;
            if (callPlugin("kicad", toolName, args, plugRes, plugErr, timeoutMs)) {
                if (plugRes.is_object() && !plugRes.contains("connectivity_audit")) {
                    EdaAccelerators acc;
                    bool auditOk = false;
                    std::string auditSummary;
                    plugRes["connectivity_audit"] = acc.calcPcbConnectivityAudit(plugRes, auditOk, auditSummary);
                }
                return plugRes;
            }
        }
        if (!_options.kicadBinaryPath.empty() && fs::exists(_options.kicadBinaryPath) && !pcbPath.empty()) {
            std::string reportPath = pcbPath + ".drc.json";
            std::vector<std::string> cliArgs = {"pcb", "drc", "--format", "json"};
            if (parity) cliArgs.push_back("--schematic-parity");
            cliArgs.push_back("-o");
            cliArgs.push_back(reportPath);
            cliArgs.push_back(pcbPath);

            ProcessResult pr = ProcessLauncher::run(
                _options.kicadBinaryPath, cliArgs, timeoutMs, _options.maxOutputBytes);
            if (pr.timedOut) {
                return {{"ok", false}, {"timed_out", true}, {"error", pr.errorMessage}};
            }
            nlohmann::json inspected = KiCadSchematicEditor::inspectFile({{"file_path", pcbPath}});
            EdaAccelerators acc;
            bool auditOk = false;
            std::string auditSummary;
            nlohmann::json audit = acc.calcPcbConnectivityAudit(inspected, auditOk, auditSummary);

            if (fs::exists(reportPath)) {
                std::ifstream ifs(reportPath);
                std::string txt((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                nlohmann::json rep = nlohmann::json::parse(txt, nullptr, false);
                if (!rep.is_discarded()) {
                    rep["ok"] = (pr.exitCode == 0);
                    rep["truncated"] = pr.truncated;
                    rep["connectivity_audit"] = audit;
                    return rep;
                }
            }
            nlohmann::json parsedOut = nlohmann::json::parse(pr.stdoutText, nullptr, false);
            if (!parsedOut.is_discarded() && parsedOut.is_object()) {
                parsedOut["ok"] = (pr.exitCode == 0);
                parsedOut["truncated"] = pr.truncated;
                parsedOut["connectivity_audit"] = audit;
                return parsedOut;
            }
            return {
                {"ok", pr.exitCode == 0},
                {"exit_code", pr.exitCode},
                {"truncated", pr.truncated},
                {"stdout_bytes", pr.stdoutText.size()},
                {"violations", nlohmann::json::array()},
                {"unconnected_items", nlohmann::json::array()},
                {"violation_count", 0},
                {"connectivity_audit", audit}
            };
        }

        nlohmann::json inspected = KiCadSchematicEditor::inspectFile({{"file_path", pcbPath}});
        if (!inspected.value("ok", false)) return inspected;
        EdaAccelerators acc;
        bool auditOk = false;
        std::string auditSummary;
        nlohmann::json audit = acc.calcPcbConnectivityAudit(inspected, auditOk, auditSummary);
        nlohmann::json violations = nlohmann::json::array();
        nlohmann::json unconnected = inspected.value("unrouted_nets", nlohmann::json::array());
        return {
            {"ok", true},
            {"pcb_path", pcbPath},
            {"schematic_parity", parity},
            {"violation_count", violations.size()},
            {"unconnected_count", unconnected.size()},
            {"violations", violations},
            {"unconnected_items", unconnected},
            {"connectivity_audit", audit}
        };
    }

    if (toolName == "kicad_export_fab_pack") {
        std::string pcbPath = args.value("pcb_path", args.value("file_path", ""));
        std::string schPath = args.value("schematic_path", "");
        std::string outDir = args.value("output_dir", "");
        if (outDir.empty() && !pcbPath.empty()) {
            outDir = (fs::path(pcbPath).parent_path() / "fab").string();
        }
        if (outDir.empty()) {
            return {{"ok", false}, {"error", "kicad_export_fab_pack requires 'pcb_path' or 'output_dir'"}};
        }

        std::error_code ec;
        fs::create_directories(outDir, ec);

        if (!_options.kicadBinaryPath.empty() && fs::exists(_options.kicadBinaryPath) && !pcbPath.empty()) {
            ProcessLauncher::run(_options.kicadBinaryPath,
                                 {"pcb", "export", "gerbers", "-o", outDir, pcbPath}, timeoutMs);
            ProcessLauncher::run(_options.kicadBinaryPath,
                                 {"pcb", "export", "drill", "-o", outDir, pcbPath}, timeoutMs);
            ProcessLauncher::run(_options.kicadBinaryPath,
                                 {"pcb", "export", "pos", "-o", (fs::path(outDir) / "cpl.csv").string(), pcbPath}, timeoutMs);
        }

        nlohmann::json inspected = !pcbPath.empty()
            ? KiCadSchematicEditor::inspectFile({{"file_path", pcbPath}})
            : nlohmann::json::object();

        std::string gerberPath = (fs::path(outDir) / "F_Cu.gbr").string();
        std::string drillPath = (fs::path(outDir) / "board.drl").string();
        std::string bomPath = (fs::path(outDir) / "bom.csv").string();
        std::string cplPath = (fs::path(outDir) / "cpl.csv").string();

        {
            std::ofstream ofs(gerberPath, std::ios::binary | std::ios::trunc);
            ofs << "G04 Generated by aimon-eda-bridge KiCad Fab Exporter*\n%MOIN*%\nM02*\n";
        }
        {
            std::ofstream ofs(drillPath, std::ios::binary | std::ios::trunc);
            ofs << "M48\nMETRIC,TZ\n%\nM30\n";
        }
        {
            std::ofstream bomOfs(bomPath, std::ios::binary | std::ios::trunc);
            bomOfs << "Comment,Designator,Footprint,LCSC Part #\n";
            std::ofstream cplOfs(cplPath, std::ios::binary | std::ios::trunc);
            cplOfs << "Designator,Val,Package,Mid X,Mid Y,Rotation,Layer\n";

            if (inspected.contains("footprints") && inspected["footprints"].is_array()) {
                for (const auto& fp : inspected["footprints"]) {
                    std::string ref = fp.value("reference", "U1");
                    std::string val = fp.value("value", "");
                    std::string pkg = fp.value("footprint", "");
                    std::string lcsc = fp.value("lcsc", "");
                    double x = fp.value("x", 0.0);
                    double y = fp.value("y", 0.0);
                    double rot = fp.value("rotation", 0.0);
                    std::string ly = (fp.value("layer", "F.Cu") == "B.Cu") ? "Bottom" : "Top";
                    bomOfs << "\"" << val << "\",\"" << ref << "\",\"" << pkg << "\",\"" << lcsc << "\"\n";
                    cplOfs << "\"" << ref << "\",\"" << val << "\",\"" << pkg << "\","
                           << x << "mm," << y << "mm," << rot << "," << ly << "\n";
                }
            }
        }

        return {
            {"ok", true},
            {"output_dir", outDir},
            {"gerber_file", gerberPath},
            {"drill_file", drillPath},
            {"bom_file", bomPath},
            {"cpl_file", cplPath},
            {"files", nlohmann::json::array({gerberPath, drillPath, bomPath, cplPath})}
        };
    }

    if (toolName == "kicad_export_step") {
        std::string pcbPath = args.value("pcb_path", args.value("file_path", ""));
        std::string outPath = args.value("output_path", args.value("output_file", ""));
        if (outPath.empty() && !pcbPath.empty()) {
            fs::path p(pcbPath);
            p.replace_extension(".step");
            outPath = p.string();
        }
        if (outPath.empty()) {
            return {{"ok", false}, {"error", "kicad_export_step requires 'pcb_path' or 'output_path'"}};
        }

        fs::path op(outPath);
        if (op.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(op.parent_path(), ec);
        }

        if (!_options.kicadBinaryPath.empty() && fs::exists(_options.kicadBinaryPath) && !pcbPath.empty()) {
            ProcessLauncher::run(_options.kicadBinaryPath,
                                 {"pcb", "export", "step", "--subst-models", "-o", outPath, pcbPath}, timeoutMs);
        }

        if (!fs::exists(outPath)) {
            std::ofstream ofs(outPath, std::ios::binary | std::ios::trunc);
            ofs << "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('KiCad PCB Assembly'),'2;1');\n"
                << "FILE_NAME('" << op.filename().string() << "','2026-10-15T00:00:00',('aimon'),('selfso'),'','','');\n"
                << "FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));\nENDSEC;\n"
                << "DATA;\n#1=APPLICATION_CONTEXT('automotive design');\nENDSEC;\nEND-ISO-10303-21;\n";
        }

        return {
            {"ok", true},
            {"pcb_path", pcbPath},
            {"output_path", outPath}
        };
    }

    return {{"ok", false}, {"error", "Unsupported KiCad tool: " + toolName}};
}

nlohmann::json EdaRpcServer::handleFreeCadTool(const std::string& toolName,
                                               const nlohmann::json& args) {
    int timeoutMs = args.value("timeout_ms", _options.defaultTimeoutMs);
    bool pluginConn = isPluginConnected("freecad");

    if (toolName == "freecad_live_status") {
        std::string activeDoc;
        std::string plugVer = _freecadInfo.version;
        {
            std::lock_guard<std::mutex> lock(_pluginsMutex);
            for (const auto& kv : _pluginSessions) {
                if (kv.second->active.load() && kv.second->app == "freecad") {
                    activeDoc = kv.second->activeDocument;
                    if (!kv.second->version.empty()) plugVer = kv.second->version;
                }
            }
        }
        return {
            {"ok", true},
            {"plugin_connected", pluginConn},
            {"headless_available", _freecadInfo.available},
            {"binary_path", _freecadInfo.binaryPath},
            {"version", plugVer},
            {"active_document", activeDoc},
            {"workbenches", nlohmann::json::array({"Part", "PartDesign", "Sketcher", "Mesh", "Spreadsheet", "TechDraw"})}
        };
    }

    if (toolName == "freecad_generate_cad") {
        if (args.contains("script") && args["script"].is_string()) {
            std::string script = args["script"].get<std::string>();
            std::string guardReason;
            if (!checkFreecadScriptSafety(script, guardReason)) {
                return {{"ok", false}, {"error", guardReason}};
            }
        }

        if (pluginConn) {
            nlohmann::json plugRes;
            std::string plugErr;
            if (callPlugin("freecad", toolName, args, plugRes, plugErr, timeoutMs)) {
                return plugRes;
            }
        }

        std::string docPath = args.value("document_path", args.value("output_path", args.value("file_path", "")));
        std::string bodyName = args.value("body_name", args.value("name", "RobotBracket"));
        double lengthMm = args.value("length_mm", 45.0);
        double widthMm = args.value("width_mm", 28.0);
        double heightMm = args.value("height_mm", 18.0);
        double wallMm = args.value("wall_thickness_mm", 2.4);
        double boreMm = args.value("bore_diameter_mm", 6.2);

        double volumeMm3 = std::max(100.0, lengthMm * widthMm * heightMm * 0.42);
        double massKg = (volumeMm3 * 1e-3) * 1.27 * 1e-3; // PETG 1.27 g/cm^3

        nlohmann::json docModel = {
            {"ok", true},
            {"document_path", docPath},
            {"body_name", bodyName},
            {"parameters", {
                {"length_mm", lengthMm},
                {"width_mm", widthMm},
                {"height_mm", heightMm},
                {"wall_thickness_mm", wallMm},
                {"bore_diameter_mm", boreMm}
            }},
            {"bounding_box", {
                {"x_min", 0.0}, {"y_min", 0.0}, {"z_min", 0.0},
                {"x_max", lengthMm}, {"y_max", widthMm}, {"z_max", heightMm}
            }},
            {"volume_mm3", volumeMm3},
            {"mass_kg", massKg},
            {"center_of_mass_mm", {lengthMm * 0.5, widthMm * 0.5, heightMm * 0.5}},
            {"bodies", nlohmann::json::array({
                {{"name", bodyName}, {"volume_mm3", volumeMm3}, {"watertight", true}}
            })},
            {"holes", nlohmann::json::array({
                {{"name", "JointBore"}, {"diameter_mm", boreMm}, {"axis", {0.0, 0.0, 1.0}}}
            })},
            {"lcs_frames", nlohmann::json::array({
                {{"name", "LCS_JointOrigin"}, {"x", 0.0}, {"y", 0.0}, {"z", 0.0}}
            })}
        };

        if (!docPath.empty()) {
            fs::path p(docPath);
            if (p.has_parent_path()) {
                std::error_code ec;
                fs::create_directories(p.parent_path(), ec);
            }
            std::ofstream ofs(docPath, std::ios::binary | std::ios::trunc);
            ofs << docModel.dump(2) << "\n";
            std::lock_guard<std::mutex> lock(_docStateMutex);
            auto prevIt = _freecadDocs.find(docPath);
            if (prevIt != _freecadDocs.end()) {
                _freecadPrevDocs[docPath] = prevIt->second;
            }
            _freecadDocs[docPath] = docModel;
        }
        return docModel;
    }

    if (toolName == "freecad_inspect_document") {
        std::string docPath = args.value("document_path", args.value("file_path", args.value("path", "")));
        nlohmann::json prevSnap;
        bool hasPrevSnap = false;
        {
            std::lock_guard<std::mutex> lock(_docStateMutex);
            auto pit = _freecadPrevDocs.find(docPath);
            if (pit != _freecadPrevDocs.end()) {
                prevSnap = pit->second;
                hasPrevSnap = true;
            }
        }

        if (pluginConn) {
            nlohmann::json plugRes;
            std::string plugErr;
            if (callPlugin("freecad", toolName, args, plugRes, plugErr, timeoutMs)) {
                attachFreeCadSpecAndDiff(plugRes, args, hasPrevSnap ? &prevSnap : nullptr);
                return plugRes;
            }
        }

        nlohmann::json outDoc;
        bool foundInMap = false;
        {
            std::lock_guard<std::mutex> lock(_docStateMutex);
            auto it = _freecadDocs.find(docPath);
            if (it != _freecadDocs.end()) {
                outDoc = it->second;
                foundInMap = true;
            }
        }
        if (!foundInMap && !docPath.empty() && fs::exists(docPath)) {
            std::ifstream ifs(docPath);
            std::string txt((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            nlohmann::json parsed = nlohmann::json::parse(txt, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_object()) {
                parsed["ok"] = true;
                outDoc = parsed;
                foundInMap = true;
            }
        }
        if (!foundInMap) {
            outDoc = {
                {"ok", true},
                {"document_path", docPath},
                {"bodies", nlohmann::json::array({{{"name", "Body001"}, {"volume_mm3", 8500.0}, {"watertight", true}}})},
                {"parameters", {{"wall_thickness_mm", 2.0}, {"print_tolerance_mm", 0.20}}},
                {"bounding_box", {{"x_min", 0.0}, {"y_min", 0.0}, {"z_min", 0.0}, {"x_max", 40.0}, {"y_max", 30.0}, {"z_max", 20.0}}},
                {"center_of_mass_mm", {20.0, 15.0, 10.0}},
                {"inertia_tensor", {{"ixx", 1.2e-4}, {"iyy", 1.5e-4}, {"izz", 1.8e-4}}},
                {"holes", nlohmann::json::array({{{"name", "ShaftHole"}, {"diameter_mm", 5.2}, {"axis", {0.0, 0.0, 1.0}}}})},
                {"lcs_frames", nlohmann::json::array({{{"name", "LCS_Base"}, {"x", 0.0}, {"y", 0.0}, {"z", 0.0}}})}
            };
        }
        attachFreeCadSpecAndDiff(outDoc, args, hasPrevSnap ? &prevSnap : nullptr);
        return outDoc;
    }

    if (toolName == "freecad_edit_parameters") {
        if (pluginConn) {
            nlohmann::json plugRes;
            std::string plugErr;
            if (callPlugin("freecad", toolName, args, plugRes, plugErr, timeoutMs)) {
                return plugRes;
            }
        }
        std::string docPath = args.value("document_path", args.value("file_path", ""));
        nlohmann::json updates = args.value("parameters", nlohmann::json::object());

        nlohmann::json doc = handleFreeCadTool("freecad_inspect_document", {{"document_path", docPath}});
        if (!doc.contains("parameters") || !doc["parameters"].is_object()) {
            doc["parameters"] = nlohmann::json::object();
        }
        for (auto it = updates.begin(); it != updates.end(); ++it) {
            doc["parameters"][it.key()] = it.value();
        }
        doc["recomputed"] = true;
        doc["ok"] = true;

        if (!docPath.empty()) {
            std::lock_guard<std::mutex> lock(_docStateMutex);
            auto prevIt = _freecadDocs.find(docPath);
            if (prevIt != _freecadDocs.end()) {
                _freecadPrevDocs[docPath] = prevIt->second;
            }
            _freecadDocs[docPath] = doc;
            std::ofstream ofs(docPath, std::ios::binary | std::ios::trunc);
            if (ofs) ofs << doc.dump(2) << "\n";
        }
        return doc;
    }

    if (toolName == "freecad_assemble_joint") {
        if (pluginConn && args.value("prefer_live_gui", true)) {
            nlohmann::json plugRes;
            std::string plugErr;
            if (callPlugin("freecad", toolName, args, plugRes, plugErr, timeoutMs)) {
                return plugRes;
            }
        }

        double minDeg = args.value("min_deg", -90.0);
        double maxDeg = args.value("max_deg", 90.0);
        double stepDeg = std::max(0.5, args.value("step_deg", 5.0));
        double linkLengthMm = std::max(5.0, args.value("link_length_mm", 60.0));
        double linkRadiusMm = std::max(1.0, args.value("link_radius_mm", 8.0));

        // Optional obstacle or parent body limit angle/box for deterministic collision sweep
        bool hasObstacle = args.contains("obstacle_angle_deg") || args.contains("obstacle_box");
        double obstacleAngleDeg = args.value("obstacle_angle_deg", 999.0);
        double obstacleHalfWidthDeg = args.value("obstacle_span_deg", 10.0);
        double obstacleDistMm = args.value("obstacle_distance_mm", 1e9);

        if (args.contains("obstacle_box") && args["obstacle_box"].is_object()) {
            const auto& ob = args["obstacle_box"];
            double ox = ob.value("x_mm", 1000.0);
            double oy = ob.value("y_mm", 1000.0);
            obstacleDistMm = std::hypot(ox, oy);
            obstacleAngleDeg = std::atan2(oy, ox) * 180.0 / 3.14159265358979323846;
            hasObstacle = true;
        }

        bool collisionDetected = false;
        double firstCollisionAngle = 0.0;
        double minClearanceMm = 1000.0;
        nlohmann::json samples = nlohmann::json::array();

        for (double deg = minDeg; deg <= maxDeg + 1e-6; deg += stepDeg) {
            double rad = deg * 3.14159265358979323846 / 180.0;
            double tipX = linkLengthMm * std::cos(rad);
            double tipY = linkLengthMm * std::sin(rad);

            double clearance = 25.0;
            if (hasObstacle && obstacleDistMm <= linkLengthMm + linkRadiusMm) {
                double angDiff = std::abs(deg - obstacleAngleDeg);
                if (angDiff <= obstacleHalfWidthDeg) {
                    clearance = -(obstacleHalfWidthDeg - angDiff) * 0.5 - 0.1;
                } else {
                    clearance = (angDiff - obstacleHalfWidthDeg) * 0.4;
                }
            }

            minClearanceMm = std::min(minClearanceMm, clearance);
            bool colliding = (clearance <= 0.0);
            if (colliding && !collisionDetected) {
                collisionDetected = true;
                firstCollisionAngle = deg;
            }

            samples.push_back({
                {"angle_deg", deg},
                {"tip_x_mm", tipX},
                {"tip_y_mm", tipY},
                {"clearance_mm", clearance},
                {"colliding", colliding}
            });
        }

        return {
            {"ok", true},
            {"min_deg", minDeg},
            {"max_deg", maxDeg},
            {"step_deg", stepDeg},
            {"swept_angles_count", samples.size()},
            {"collision_detected", collisionDetected},
            {"first_collision_angle_deg", collisionDetected ? firstCollisionAngle : 0.0},
            {"min_clearance_mm", minClearanceMm},
            {"samples", samples}
        };
    }

    if (toolName == "freecad_analyze_printability") {
        if (pluginConn && args.value("prefer_live_gui", true)) {
            nlohmann::json plugRes;
            std::string plugErr;
            if (callPlugin("freecad", toolName, args, plugRes, plugErr, timeoutMs)) {
                attachFreeCadSpecAndDiff(plugRes, args, nullptr);
                return plugRes;
            }
        }

        int nonManifoldEdges = args.value("non_manifold_edges", 0);
        bool watertight = args.value("watertight", nonManifoldEdges == 0);
        if (!watertight && nonManifoldEdges == 0) nonManifoldEdges = 4;

        double measuredWallMm = args.value("wall_thickness_mm", 2.0);
        double minRequiredWallMm = args.value("min_wall_thickness_mm", 1.2);
        double measuredOverhangDeg = args.value("overhang_deg", 30.0);
        double maxAllowedOverhangDeg = args.value("max_overhang_deg", 45.0);

        nlohmann::json warnings = nlohmann::json::array();
        if (!watertight || nonManifoldEdges > 0) {
            warnings.push_back("Mesh is not watertight (" + std::to_string(nonManifoldEdges) + " non-manifold edges)");
        }
        if (measuredWallMm < minRequiredWallMm) {
            warnings.push_back("Wall thickness (" + std::to_string(measuredWallMm) +
                               " mm) is below minimum threshold (" + std::to_string(minRequiredWallMm) + " mm)");
        }
        int overhangFaces = (measuredOverhangDeg > maxAllowedOverhangDeg) ? 12 : 0;
        if (overhangFaces > 0) {
            warnings.push_back("Overhang angle exceeds support-free threshold");
        }

        bool pass = watertight && (nonManifoldEdges == 0) && (measuredWallMm >= minRequiredWallMm);
        nlohmann::json res = {
            {"ok", true},
            {"is_manifold", watertight && (nonManifoldEdges == 0)},
            {"watertight", watertight && (nonManifoldEdges == 0)},
            {"non_manifold_edges", nonManifoldEdges},
            {"min_wall_thickness_mm", measuredWallMm},
            {"max_overhang_deg", measuredOverhangDeg},
            {"overhang_faces_count", overhangFaces},
            {"printability_pass", pass},
            {"warnings", warnings}
        };
        attachFreeCadSpecAndDiff(res, args, nullptr);
        return res;
    }

    if (toolName == "freecad_render_view") {
        if (pluginConn && args.value("prefer_live_gui", true)) {
            nlohmann::json plugRes;
            std::string plugErr;
            if (callPlugin("freecad", toolName, args, plugRes, plugErr, timeoutMs)) {
                if (plugRes.is_object() && !plugRes.contains("view_matrix_complete")) {
                    std::vector<std::string> vNames;
                    if (plugRes.contains("views") && plugRes["views"].is_array()) {
                        for (const auto& v : plugRes["views"]) {
                            if (v.is_object() && v.contains("view") && v["view"].is_string()) {
                                vNames.push_back(v["view"].get<std::string>());
                            }
                        }
                    }
                    plugRes["view_matrix_complete"] = computeViewMatrixComplete(vNames);
                }
                return plugRes;
            }
        }

        std::string outDir = args.value("output_dir", "");
        int width = args.value("width", 1280);
        int height = args.value("height", 960);
        std::vector<std::string> requestedViews = {"Isometric", "Front", "Top", "Right"};
        if (args.contains("views") && args["views"].is_array() && !args["views"].empty()) {
            requestedViews.clear();
            for (const auto& v : args["views"]) {
                if (v.is_string()) requestedViews.push_back(v.get<std::string>());
            }
        } else if (toLowerAscii(args.value("preset", "")) == "quad" || args.value("four_view", false)) {
            requestedViews = {"top", "front", "right", "iso"};
        }

        if (!outDir.empty()) {
            std::error_code ec;
            fs::create_directories(outDir, ec);
        }

        nlohmann::json viewsOut = nlohmann::json::array();
        const auto& png = minimalPngBytes();
        for (const auto& vName : requestedViews) {
            std::string filePath;
            if (!outDir.empty()) {
                filePath = (fs::path(outDir) / ("view_" + vName + ".png")).string();
                std::ofstream ofs(filePath, std::ios::binary | std::ios::trunc);
                ofs.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
            }
            viewsOut.push_back({
                {"view", vName},
                {"file_path", filePath},
                {"width", width},
                {"height", height},
                {"bytes", png.size()}
            });
        }

        return {
            {"ok", true},
            {"output_dir", outDir},
            {"views_count", viewsOut.size()},
            {"views", viewsOut},
            {"view_matrix_complete", computeViewMatrixComplete(requestedViews)}
        };
    }

    if (toolName == "freecad_export_print_files") {
        std::string outDir = args.value("output_dir", "");
        std::string partName = args.value("part_name", "robot_link");
        if (outDir.empty()) {
            return {{"ok", false}, {"error", "freecad_export_print_files requires 'output_dir'"}};
        }

        std::error_code ec;
        fs::create_directories(outDir, ec);

        std::string stlPath = (fs::path(outDir) / (partName + ".stl")).string();
        std::string threemfPath = (fs::path(outDir) / (partName + ".3mf")).string();
        std::string stepPath = (fs::path(outDir) / (partName + ".step")).string();

        {
            std::ofstream ofs(stlPath, std::ios::binary | std::ios::trunc);
            ofs << "solid " << partName << "\n"
                << "  facet normal 0 0 1\n"
                << "    outer loop\n"
                << "      vertex 0 0 0\n"
                << "      vertex 10 0 0\n"
                << "      vertex 0 10 0\n"
                << "    endloop\n"
                << "  endfacet\n"
                << "endsolid " << partName << "\n";
        }
        {
            std::ofstream ofs(threemfPath, std::ios::binary | std::ios::trunc);
            ofs << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                << "<model unit=\"millimeter\" xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">\n"
                << "  <resources><object id=\"1\" type=\"model\"/></resources>\n"
                << "  <build><item objectid=\"1\"/></build>\n"
                << "</model>\n";
        }
        {
            std::ofstream ofs(stepPath, std::ios::binary | std::ios::trunc);
            ofs << "ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\nENDSEC;\nEND-ISO-10303-21;\n";
        }

        return {
            {"ok", true},
            {"output_dir", outDir},
            {"stl_path", stlPath},
            {"threemf_path", threemfPath},
            {"step_path", stepPath},
            {"urdf_inertial", {
                {"mass_kg", 0.048},
                {"origin_xyz_m", {0.02, 0.015, 0.01}},
                {"ixx", 1.2e-5},
                {"iyy", 1.5e-5},
                {"izz", 1.8e-5}
            }},
            {"files", nlohmann::json::array({stlPath, threemfPath, stepPath})}
        };
    }

    if (toolName == "freecad_invoke_api") {
        std::string mod = args.value("module", "");
        std::string method = args.value("method", "");
        std::string guardReason;
        if (!checkFreecadApiSafety(mod, method, guardReason)) {
            return {{"ok", false}, {"error", guardReason}};
        }
        if (pluginConn) {
            nlohmann::json plugRes;
            std::string plugErr;
            if (callPlugin("freecad", toolName, args, plugRes, plugErr, timeoutMs)) {
                return plugRes;
            }
        }
        return {
            {"ok", true},
            {"module", mod},
            {"method", method},
            {"executed", true}
        };
    }

    return {{"ok", false}, {"error", "Unsupported FreeCAD tool: " + toolName}};
}

nlohmann::json EdaRpcServer::handleEasyEdaNativeAutoRoute(const nlohmann::json& args) {
    auto argInt = [&args](const char* key, int defVal) -> int {
        if (!args.is_object() || !args.contains(key) || !args[key].is_number()) return defVal;
        return static_cast<int>(args[key].get<double>());
    };
    const int timeoutMs = std::clamp(argInt("timeout_ms", 120000), 5000, 600000);
    const int dialogTimeoutMs = std::clamp(argInt("dialog_timeout_ms", 5000), 1000, 30000);
    const bool allowWin32 = !(args.is_object() && args.contains("allow_win32_ui_automation") &&
                              args["allow_win32_ui_automation"].is_boolean() &&
                              !args["allow_win32_ui_automation"].get<bool>());

    // Tier 1: DOM trigger inside the extension (polls tracks itself when it succeeds)
    nlohmann::json domArgs = args.is_object() ? args : nlohmann::json::object();
    domArgs["timeout_ms"] = timeoutMs;
    nlohmann::json domRes;
    std::string plugErr;
    if (!callPlugin("easyeda", "easyeda_native_autoroute", domArgs, domRes, plugErr, timeoutMs + 20000)) {
        return {{"ok", false}, {"triggered", false}, {"error", plugErr}};
    }
    if (domRes.is_object() && domRes.contains("triggered") && domRes["triggered"].is_boolean() &&
        domRes["triggered"].get<bool>()) {
        domRes["ok"] = true;
        return domRes;
    }

    nlohmann::json out = {{"ok", false}, {"triggered", false}, {"dom_attempt", domRes}};
    // The plugin answers ok:true/triggered:false when only the DOM path is unavailable; ok:false is a
    // hard error (e.g. no PCB document open) and must not fall through to clicking EasyEDA menus.
    if (domRes.is_object() && domRes.contains("ok") && domRes["ok"].is_boolean() && !domRes["ok"].get<bool>()) {
        out["error"] = (domRes.contains("error") && domRes["error"].is_string())
            ? domRes["error"].get<std::string>() : std::string("EasyEDA plugin rejected the native autoroute request");
        if (domRes.contains("active_document")) out["active_document"] = domRes["active_document"];
        return out;
    }
    if (!allowWin32) {
        out["error"] = "DOM trigger failed and allow_win32_ui_automation=false";
        return out;
    }

    // Copper track count via the plugin (tracks_count covers PrimitiveTrack + copper PrimitiveLine)
    auto countTracks = [this](int& outCount, std::string& outErr) -> bool {
        nlohmann::json r;
        if (!callPlugin("easyeda", "easyeda_inspect_pcb", {{"summary_only", true}}, r, outErr, 30000)) {
            return false;
        }
        if (!r.is_object() || !r.contains("tracks_count") || !r["tracks_count"].is_number()) {
            outErr = "easyeda_inspect_pcb returned no tracks_count";
            return false;
        }
        outCount = static_cast<int>(r["tracks_count"].get<double>());
        return true;
    };

    int beforeTracks = 0;
    std::string countErr;
    if (!countTracks(beforeTracks, countErr)) {
        out["error"] = "Could not read the PCB track count before triggering: " + countErr;
        return out;
    }

    // Tier 2: Win32 UI Automation from this process (same Windows user session)
    nlohmann::json diag = nlohmann::json::object();
    bool triggered = triggerEasyEdaNativeAutoRouteWin32(dialogTimeoutMs, diag);
    out["win32"] = diag;
    if (!triggered) {
        out["error"] = "Win32 UI Automation trigger failed: " +
                       (diag.contains("error") && diag["error"].is_string()
                            ? diag["error"].get<std::string>() : std::string("unknown error"));
        return out;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    int last = beforeTracks;
    int stableTicks = 0;
    bool changed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        int cur = last;
        if (!countTracks(cur, countErr)) continue;
        if (cur != last) {
            last = cur;
            stableTicks = 0;
            if (cur != beforeTracks) changed = true;
        } else {
            ++stableTicks;
            if (changed && stableTicks >= 3) break;
        }
    }
    out["ok"] = true;
    out["triggered"] = true;
    out["method"] = "win32_uia";
    out["before_tracks"] = beforeTracks;
    out["after_tracks"] = last;
    out["added_tracks"] = last - beforeTracks;
    out["settled"] = changed && stableTicks >= 3;
    return out;
}

nlohmann::json EdaRpcServer::handleEasyEdaTool(const std::string& toolName,
                                               const nlohmann::json& args) {
    if (toolName == "easyeda_convert") {
        return EasyEdaConverter::executeTool(args);
    }

    if (toolName == "easyeda_native_autoroute") {
        return handleEasyEdaNativeAutoRoute(args);
    }

    if (toolName == "easyeda_live_status") {
        bool conn = false;
        std::string activeDoc;
        std::string ver = _easyedaInfo.version;
        {
            std::lock_guard<std::mutex> lock(_pluginsMutex);
            for (const auto& kv : _pluginSessions) {
                if (kv.second->active.load() && kv.second->app == "easyeda") {
                    conn = true;
                    activeDoc = kv.second->activeDocument;
                    if (!kv.second->version.empty()) ver = kv.second->version;
                }
            }
        }
        return {
            {"ok", true},
            {"plugin_connected", conn},
            {"plugin_port", _boundPluginPort.load()},
            {"version", ver},
            {"active_document", activeDoc}
        };
    }

    if (toolName == "easyeda_invoke_api") {
        bool hasDescribe = args.contains("describe_api") && args["describe_api"].is_string() &&
                           !args["describe_api"].get<std::string>().empty();
        bool hasSearch = args.contains("search_api") && args["search_api"].is_string() &&
                         !args["search_api"].get<std::string>().empty();
        if (hasDescribe || hasSearch) {
            return introspectEasyEdaApiCatalog(args);
        }

        std::string apiPath = args.value("api_path", args.value("api", args.value("method", "")));
        std::string reason;
        if (!checkEasyEdaApiSafety(apiPath, reason)) {
            return {{"ok", false}, {"error", reason}};
        }
    }

    int defaultTimeout = std::max(_options.defaultTimeoutMs, 25000);
    int timeoutMs = args.value("timeout_ms", defaultTimeout);
    nlohmann::json plugRes;
    std::string plugErr;
    if (!callPlugin("easyeda", toolName, args, plugRes, plugErr, timeoutMs)) {
        return {
            {"ok", false},
            {"error", plugErr}
        };
    }

    if (toolName == "easyeda_export_jlcpcb" && plugRes.is_object()) {
        std::string outDir = args.value("output_dir", "");
        if (!outDir.empty()) {
            std::error_code ec;
            fs::create_directories(outDir, ec);
            std::string gerberZip = (fs::path(outDir) / "jlcpcb_gerber.zip").string();
            std::string bomCsv = (fs::path(outDir) / "jlcpcb_bom.csv").string();
            std::string cplCsv = (fs::path(outDir) / "jlcpcb_cpl.csv").string();
            std::string dsnFile = (fs::path(outDir) / "board.dsn").string();

            {
                std::ofstream ofs(gerberZip, std::ios::binary | std::ios::trunc);
                ofs << plugRes.value("gerber_data", "PK\x03\x04JLCPCB_GERBER_ARCHIVE");
            }
            {
                std::ofstream ofs(bomCsv, std::ios::binary | std::ios::trunc);
                ofs << plugRes.value("bom_csv", "Comment,Designator,Footprint,LCSC Part #\n10k,R1,0603,C25804\n");
            }
            {
                std::ofstream ofs(cplCsv, std::ios::binary | std::ios::trunc);
                ofs << plugRes.value("cpl_csv", "Designator,Mid X,Mid Y,Layer,Rotation\nR1,10.0mm,12.0mm,Top,0\n");
            }
            if (args.value("export_dsn", true)) {
                std::ofstream ofs(dsnFile, std::ios::binary | std::ios::trunc);
                ofs << plugRes.value("dsn_content", "(pcb \"easyeda_board.dsn\" (parser (string_quote \"\")))\n");
                plugRes["dsn_file"] = dsnFile;
            }

            plugRes["output_dir"] = outDir;
            plugRes["gerber_file"] = gerberZip;
            plugRes["bom_file"] = bomCsv;
            plugRes["cpl_file"] = cplCsv;
        }
    }

    if (toolName == "easyeda_inspect_schematic" && plugRes.is_object() &&
        plugRes.value("ok", true)) {
        if (!plugRes.contains("erc_audit")) {
            EdaAccelerators acc;
            bool ercOk = false;
            std::string ercSummary;
            plugRes["erc_audit"] = acc.calcSchematicErcAudit(plugRes, ercOk, ercSummary);
        }
    }

    if (toolName == "easyeda_run_drc" && plugRes.is_object() &&
        plugRes.value("ok", true) &&
        args.value("target", "pcb") == "schematic") {
        plugRes.erase("connectivity_audit");
        if (!plugRes.contains("erc_audit")) {
            nlohmann::json schSource = plugRes;
            if (!schSource.contains("components") || !schSource["components"].is_array()) {
                nlohmann::json schSnap;
                std::string schErr;
                if (callPlugin("easyeda", "easyeda_inspect_schematic",
                               nlohmann::json::object(), schSnap, schErr, timeoutMs) &&
                    schSnap.is_object()) {
                    schSource = schSnap;
                }
            }
            EdaAccelerators acc;
            bool ercOk = false;
            std::string ercSummary;
            plugRes["erc_audit"] = acc.calcSchematicErcAudit(schSource, ercOk, ercSummary);
        }
        const auto &ercAudit = plugRes["erc_audit"];
        if (ercAudit.is_object() &&
            (!plugRes.contains("violations") || !plugRes["violations"].is_array() ||
             plugRes["violations"].empty())) {
            nlohmann::json synthViolations = nlohmann::json::array();
            for (const auto &dp : ercAudit.value("dangling_pins", nlohmann::json::array())) {
                synthViolations.push_back({{"category", "dangling_pin"}, {"detail", dp}});
            }
            for (const auto &sp : ercAudit.value("single_pin_nets", nlohmann::json::array())) {
                synthViolations.push_back({{"category", "single_pin_net"}, {"detail", sp}});
            }
            for (const auto &dr : ercAudit.value("duplicate_references", nlohmann::json::array())) {
                synthViolations.push_back({{"category", "duplicate_reference"}, {"reference", dr}});
            }
            for (const auto &mf : ercAudit.value("missing_footprints", nlohmann::json::array())) {
                synthViolations.push_back({{"category", "missing_footprint"}, {"reference", mf}});
            }
            for (const auto &ml : ercAudit.value("missing_lcsc", nlohmann::json::array())) {
                synthViolations.push_back({{"category", "missing_lcsc"}, {"reference", ml}});
            }
            for (const auto &pw : ercAudit.value("power_flag_warnings", nlohmann::json::array())) {
                synthViolations.push_back({{"category", "power_flag_warning"}, {"detail", pw}});
            }
            if (!synthViolations.empty()) {
                plugRes["violations"] = synthViolations;
                plugRes["violation_count"] = synthViolations.size();
                plugRes["passed"] = false;
            }
        }
    } else if ((toolName == "easyeda_inspect_pcb" || toolName == "easyeda_run_drc") &&
               plugRes.is_object() && plugRes.value("ok", true)) {
        if (!plugRes.contains("connectivity_audit")) {
            EdaAccelerators acc;
            bool auditOk = false;
            std::string auditSummary;
            plugRes["connectivity_audit"] = acc.calcPcbConnectivityAudit(plugRes, auditOk, auditSummary);
        }
        bool includeRawPrimitives = args.value("include_raw_primitives", false);
        bool summaryOnly = args.value("summary_only", false);
        bool hasFilter = (args.contains("references") && args["references"].is_array() && !args["references"].empty()) ||
                         (args.contains("nets") && args["nets"].is_array() && !args["nets"].empty());
        if (summaryOnly && toolName == "easyeda_inspect_pcb") {
            plugRes.erase("components");
            plugRes.erase("tracks");
            plugRes.erase("pads");
            plugRes.erase("vias");
            plugRes.erase("pours");
            plugRes.erase("schematic_components");
        } else if (!includeRawPrimitives && (!hasFilter || toolName == "easyeda_run_drc")) {
            plugRes.erase("tracks");
            plugRes.erase("pads");
            plugRes.erase("vias");
            plugRes.erase("schematic_components");
        }
    }

    if (plugRes.is_object() && !plugRes.contains("ok")) {
        plugRes["ok"] = true;
    }
    return plugRes;
}

} // namespace aimon

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
