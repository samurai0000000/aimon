/*
 * WindowsInstallerMain.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "Version.hxx"

#define AIMON_WIDE_STR_(x) L##x
#define AIMON_WIDE_STR(x) AIMON_WIDE_STR_(x)

namespace fs = std::filesystem;

namespace {

constexpr const char* kInstallerMagic = "AIMON_INST_V1";
constexpr size_t kInstallerMagicLen = 13;
constexpr size_t kFooterLen = kInstallerMagicLen + sizeof(uint64_t);
static const wchar_t* kAppVersion = AIMON_WIDE_STR(AIMON_VERSION_STRING);
constexpr const wchar_t* kUninstallRegPath =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\aimon";
constexpr const wchar_t* kRunRegPath =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kSelfsoRegPath =
    L"Software\\selfso\\aimon";

uint16_t readLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readLe64(const uint8_t* p) {
    uint64_t lo = readLe32(p);
    uint64_t hi = readLe32(p + 4);
    return lo | (hi << 32);
}

std::wstring getEnvWide(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) {
        return L"";
    }
    return std::wstring(buf, n);
}

std::wstring queryInstalledVersion() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallRegPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return L"";
    }
    wchar_t buf[128] = {0};
    DWORD bufBytes = sizeof(buf);
    DWORD type = 0;
    std::wstring result;
    if (RegQueryValueExW(hKey, L"DisplayVersion", nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf), &bufBytes) == ERROR_SUCCESS &&
        type == REG_SZ) {
        result = buf;
    }
    RegCloseKey(hKey);
    return result;
}

void runHiddenCommand(std::wstring cmdLine, bool wait) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    std::memset(&si, 0, sizeof(si));
    std::memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    std::wstring workDir = getEnvWide(L"LOCALAPPDATA");
    const wchar_t* lpWorkDir = workDir.empty() ? nullptr : workDir.c_str();

    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');

    if (CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, lpWorkDir, &si, &pi)) {
        if (wait) {
            WaitForSingleObject(pi.hProcess, 15000);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void stopRunningInstances() {
    runHiddenCommand(L"taskkill.exe /F /IM aimon.exe", true);
    runHiddenCommand(L"taskkill.exe /F /IM aimon-eda-bridge.exe", true);
}

void cleanStaleInstallFiles(const fs::path& destDir) {
    std::error_code ec;
    if (!fs::exists(destDir, ec)) {
        return;
    }
    for (const auto& entry : fs::recursive_directory_iterator(destDir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        std::wstring ext = entry.path().extension().wstring();
        for (wchar_t& c : ext) {
            if (c >= L'A' && c <= L'Z') {
                c = static_cast<wchar_t>(c - L'A' + L'a');
            }
        }
        if (ext == L".dll" || ext == L".exe" || ext == L".eext") {
            SetFileAttributesW(entry.path().wstring().c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(entry.path().wstring().c_str());
        }
    }
}

bool writeExactFileWin32(const fs::path& outPath, const void* data, size_t size) {
    std::error_code ec;
    fs::create_directories(outPath.parent_path(), ec);
    const std::wstring wpath = outPath.wstring();
    SetFileAttributesW(wpath.c_str(), FILE_ATTRIBUTE_NORMAL);
    HANDLE hOut = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE) {
        return false;
    }
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    size_t remaining = size;
    while (remaining > 0) {
        DWORD chunk = static_cast<DWORD>(remaining > 0x1000000ULL ? 0x1000000ULL : remaining);
        DWORD written = 0;
        if (!WriteFile(hOut, ptr, chunk, &written, nullptr) || written == 0) {
            CloseHandle(hOut);
            return false;
        }
        ptr += written;
        remaining -= written;
    }
    CloseHandle(hOut);
    return true;
}

bool createShortcut(const fs::path& shortcutPath,
                    const fs::path& targetExe,
                    const fs::path& workingDir,
                    const wchar_t* description) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = SUCCEEDED(hr);

    IShellLinkW* psl = nullptr;
    hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IShellLinkW, reinterpret_cast<LPVOID*>(&psl));
    if (FAILED(hr) || !psl) {
        if (needUninit) CoUninitialize();
        return false;
    }

    psl->SetPath(targetExe.wstring().c_str());
    psl->SetWorkingDirectory(workingDir.wstring().c_str());
    psl->SetDescription(description);
    psl->SetIconLocation(targetExe.wstring().c_str(), 0);

    IPersistFile* ppf = nullptr;
    hr = psl->QueryInterface(IID_IPersistFile, reinterpret_cast<LPVOID*>(&ppf));
    bool saved = false;
    if (SUCCEEDED(hr) && ppf) {
        std::error_code ec;
        fs::create_directories(shortcutPath.parent_path(), ec);
        if (SUCCEEDED(ppf->Save(shortcutPath.wstring().c_str(), TRUE))) {
            saved = true;
        }
        ppf->Release();
    }
    psl->Release();
    if (needUninit) CoUninitialize();
    return saved;
}

void registerUninstallKeys(const fs::path& installDir) {
    const std::wstring installDirW = installDir.wstring();
    const std::wstring exePathW = (installDir / "aimon.exe").wstring();
    const std::wstring uninstCmdW =
        L"\"" + (installDir / "aimon-uninstall.exe").wstring() + L"\" --uninstall";

    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallRegPath, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        auto setStr = [&](const wchar_t* name, const std::wstring& val) {
            RegSetValueExW(hKey, name, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(val.c_str()),
                           static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
        };
        setStr(L"DisplayName", L"aimon");
        setStr(L"DisplayVersion", kAppVersion);
        setStr(L"Publisher", L"Charles Chiou");
        setStr(L"InstallLocation", installDirW);
        setStr(L"DisplayIcon", exePathW);
        setStr(L"UninstallString", uninstCmdW);

        DWORD one = 1;
        RegSetValueExW(hKey, L"NoModify", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&one), sizeof(DWORD));
        RegSetValueExW(hKey, L"NoRepair", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&one), sizeof(DWORD));
        RegCloseKey(hKey);
    }

    HKEY hSelfso = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSelfsoRegPath, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hSelfso, nullptr) == ERROR_SUCCESS) {
        std::wstring verW = kAppVersion;
        RegSetValueExW(hSelfso, L"Version", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(verW.c_str()),
                       static_cast<DWORD>((verW.size() + 1) * sizeof(wchar_t)));
        RegSetValueExW(hSelfso, L"InstallDir", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(installDirW.c_str()),
                       static_cast<DWORD>((installDirW.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hSelfso);
    }
}

bool extractZipBufferToDir(const uint8_t* zipData,
                           size_t zipSize,
                           const fs::path& destDir,
                           std::wstring& outError) {
    cleanStaleInstallFiles(destDir);

    size_t pos = 0;
    size_t extractedFiles = 0;
    while (pos + 4 <= zipSize) {
        const uint32_t sig = readLe32(zipData + pos);
        if (sig == 0x02014b50U || sig == 0x06054b50U) {
            // Reached Central Directory or EOCD
            break;
        }
        if (sig != 0x04034b50U) {
            outError = L"Corrupt local file header inside embedded ZIP archive.";
            return false;
        }
        if (pos + 30 > zipSize) {
            outError = L"Truncated local file header inside embedded ZIP archive.";
            return false;
        }

        const uint16_t method = readLe16(zipData + pos + 8);
        const uint32_t compSize = readLe32(zipData + pos + 18);
        const uint16_t nameLen = readLe16(zipData + pos + 26);
        const uint16_t extraLen = readLe16(zipData + pos + 28);

        const size_t dataOffset = pos + 30 + nameLen + extraLen;
        if (dataOffset + compSize > zipSize) {
            outError = L"Truncated file entry data inside embedded ZIP archive.";
            return false;
        }
        if (method != 0) {
            outError = L"Unsupported compression method in embedded ZIP archive (expected STORE).";
            return false;
        }

        std::string relName(reinterpret_cast<const char*>(zipData + pos + 30), nameLen);
        if (!relName.empty() && relName.back() != '/' && relName.back() != '\\') {
            fs::path outPath = (destDir / fs::u8path(relName)).make_preferred();
            if (!writeExactFileWin32(outPath, zipData + dataOffset, compSize)) {
                outError = L"Failed to write file: " + outPath.wstring();
                return false;
            }
            ++extractedFiles;
        }

        pos = dataOffset + compSize;
    }

    if (extractedFiles == 0) {
        outError = L"Embedded archive contained zero extractable files.";
        return false;
    }
    return true;
}

bool extractAppendedZipPayload(const std::wstring& selfExePathW,
                               const fs::path& destDir,
                               std::wstring& outError) {
    // Primary path: read directly from the in-memory Win32 RT_RCDATA resource (ID 101)
    // mapped by the Windows PE loader. This requires zero file I/O on selfExePathW and
    // is immune to SMB/network drive metadata caching or SEC_IMAGE overlay locks.
    HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));
    if (hRes != nullptr) {
        HGLOBAL hData = LoadResource(nullptr, hRes);
        DWORD resSize = SizeofResource(nullptr, hRes);
        const uint8_t* resPtr =
            hData ? static_cast<const uint8_t*>(LockResource(hData)) : nullptr;
        if (resPtr != nullptr && resSize >= 30 && readLe32(resPtr) == 0x04034b50U) {
            return extractZipBufferToDir(resPtr, static_cast<size_t>(resSize), destDir, outError);
        }
    }

    // Fallback path: read the executable stream sequentially until true ReadFile EOF
    // (ignoring GetFileSizeEx, which can return stale cached sizes over SMB).
    HANDLE hFile = CreateFileW(selfExePathW.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        outError = L"Cannot open installer executable for reading.";
        return false;
    }

    std::vector<uint8_t> fileBytes;
    fileBytes.reserve(34ULL * 1024ULL * 1024ULL);
    std::vector<uint8_t> chunkBuf(1024 * 1024);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(hFile, chunkBuf.data(), static_cast<DWORD>(chunkBuf.size()), &got, nullptr)) {
            CloseHandle(hFile);
            outError = L"Failed while reading installer executable stream.";
            return false;
        }
        if (got == 0) {
            break;
        }
        fileBytes.insert(fileBytes.end(), chunkBuf.data(), chunkBuf.data() + got);
    }
    CloseHandle(hFile);

    if (fileBytes.size() < kFooterLen + 22) {
        outError = L"Installer executable is truncated.";
        return false;
    }

    const uint8_t* footer = fileBytes.data() + (fileBytes.size() - kFooterLen);
    if (std::memcmp(footer, kInstallerMagic, kInstallerMagicLen) != 0) {
        outError = L"Installer payload signature (AIMON_INST_V1) not found.";
        return false;
    }

    const uint64_t zipSize = readLe64(footer + kInstallerMagicLen);
    if (zipSize == 0 || zipSize + kFooterLen > fileBytes.size()) {
        outError = L"Invalid embedded ZIP archive size.";
        return false;
    }

    const size_t zipStart = fileBytes.size() - kFooterLen - static_cast<size_t>(zipSize);
    if (!extractZipBufferToDir(fileBytes.data() + zipStart, static_cast<size_t>(zipSize),
                               destDir, outError)) {
        return false;
    }

    fs::path uninstPath = (destDir / "aimon-uninstall.exe").make_preferred();
    std::error_code ec;
    if (!fs::exists(uninstPath, ec) && zipStart > 0 && zipStart < 16ULL * 1024ULL * 1024ULL) {
        writeExactFileWin32(uninstPath, fileBytes.data(), zipStart);
    }
    return true;
}

int performUninstall(bool silent) {
    stopRunningInstances();

    RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallRegPath);
    HKEY hRun = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, KEY_SET_VALUE, &hRun) == ERROR_SUCCESS) {
        RegDeleteValueW(hRun, L"aimon");
        RegCloseKey(hRun);
    }

    const std::wstring appData = getEnvWide(L"APPDATA");
    if (!appData.empty()) {
        std::error_code ec;
        fs::remove(fs::path(appData) / "Microsoft" / "Windows" / "Start Menu" / "Programs" / "aimon.lnk", ec);
    }

    if (!silent) {
        MessageBoxW(nullptr,
                    L"aimon has been unregistered and stopped.\n"
                    L"User configuration under %APPDATA%\\aimon has been preserved.",
                    L"aimon Uninstaller",
                    MB_OK | MB_ICONINFORMATION);
    }
    return 0;
}

} // anonymous namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool silent = false;
    bool noLaunch = false;
    bool uninstallMode = false;

    if (argv) {
        for (int i = 1; i < argc; ++i) {
            std::wstring arg = argv[i];
            if (arg == L"--silent" || arg == L"/S" || arg == L"/silent") {
                silent = true;
            } else if (arg == L"--no-launch") {
                noLaunch = true;
            } else if (arg == L"--uninstall") {
                uninstallMode = true;
            }
        }
        LocalFree(argv);
    }

    std::wstring localAppData = getEnvWide(L"LOCALAPPDATA");
    if (!localAppData.empty()) {
        SetCurrentDirectoryW(localAppData.c_str());
    }

    if (uninstallMode) {
        return performUninstall(silent);
    }

    if (localAppData.empty()) {
        if (!silent) {
            MessageBoxW(nullptr, L"Could not resolve %LOCALAPPDATA%.", L"aimon Setup", MB_OK | MB_ICONERROR);
        }
        return 1;
    }

    const fs::path installDir = fs::path(localAppData) / "Programs" / "aimon";
    const std::wstring existingVersion = queryInstalledVersion();

    if (!silent) {
        std::wstring promptMsg;
        if (existingVersion.empty()) {
            promptMsg = L"Install aimon v";
            promptMsg += kAppVersion;
            promptMsg += L" (AI Quota Monitor & EasyEDA / KiCad / FreeCAD System Tray Bridge)?\n\n";
        } else {
            promptMsg = L"Upgrade existing aimon installation (v";
            promptMsg += existingVersion;
            promptMsg += L" -> v";
            promptMsg += kAppVersion;
            promptMsg += L")?\n\n";
        }
        promptMsg += L"Destination:\n" + installDir.wstring();

        int resp = MessageBoxW(nullptr, promptMsg.c_str(), L"aimon Setup v1.0.16",
                               MB_YESNO | MB_ICONQUESTION);
        if (resp != IDYES) {
            return 0;
        }
    }

    stopRunningInstances();

    std::error_code ec;
    fs::create_directories(installDir, ec);

    wchar_t selfPathBuf[MAX_PATH * 4] = {0};
    GetModuleFileNameW(nullptr, selfPathBuf, MAX_PATH * 4);
    const std::wstring selfExePathW(selfPathBuf);

    std::wstring extractErr;
    if (!extractAppendedZipPayload(selfExePathW, installDir, extractErr)) {
        if (!silent) {
            MessageBoxW(nullptr, extractErr.c_str(), L"aimon Setup Error", MB_OK | MB_ICONERROR);
        }
        return 1;
    }

    // Deploy companion plugins into %APPDATA%
    const std::wstring appData = getEnvWide(L"APPDATA");
    const fs::path installedAimonExe = installDir / "aimon.exe";
    if (!appData.empty() && fs::exists(installedAimonExe)) {
        std::wstring deployCmd = L"\"" + installedAimonExe.wstring() +
                                 L"\" --deploy-plugins \"" + appData + L"\"";
        runHiddenCommand(deployCmd, true);
    }

    // Register versioned Windows Uninstall metadata & Start Menu shortcut
    registerUninstallKeys(installDir);
    if (!appData.empty()) {
        const fs::path startMenuShortcut =
            fs::path(appData) / "Microsoft" / "Windows" / "Start Menu" / "Programs" / "aimon.lnk";
        createShortcut(startMenuShortcut, installedAimonExe, installDir,
                       L"aimon — AI Quota Monitor & EDA/CAD System Tray Bridge");
    }

    if (!silent && !noLaunch && fs::exists(installedAimonExe)) {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        std::memset(&si, 0, sizeof(si));
        std::memset(&pi, 0, sizeof(pi));
        si.cb = sizeof(si);
        std::wstring launchCmd = L"\"" + installedAimonExe.wstring() + L"\"";
        std::vector<wchar_t> mutableCmd(launchCmd.begin(), launchCmd.end());
        mutableCmd.push_back(L'\0');
        if (CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                           0, nullptr, installDir.wstring().c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    if (!silent) {
        std::wstring doneMsg = L"aimon v";
        doneMsg += kAppVersion;
        doneMsg += L" has been installed to:\n";
        doneMsg += installDir.wstring();
        doneMsg += L"\n\nThe aimon icon is now docked in your Windows System Tray.";
        MessageBoxW(nullptr, doneMsg.c_str(), L"aimon Setup Complete", MB_OK | MB_ICONINFORMATION);
    }

    return 0;
}

#else

int main() {
    return 0;
}

#endif // _WIN32

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
