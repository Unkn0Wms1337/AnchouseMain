#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    constexpr const wchar_t* ProcessName = L"dota2.exe";
    constexpr const wchar_t* DefaultDllName = L"anchousechanger.dll";
    constexpr const wchar_t* SteamLaunchUrl = L"steam://rungameid/570";

    std::wstring ExecutableDirectory()
    {
        wchar_t Path[MAX_PATH] = {};
        const DWORD Length = GetModuleFileNameW(nullptr, Path, MAX_PATH);
        if (Length == 0 || Length >= MAX_PATH)
            return {};

        std::wstring Result(Path, Length);
        const std::size_t Slash = Result.find_last_of(L"\\/");
        return Slash == std::wstring::npos ? std::wstring() : Result.substr(0, Slash + 1);
    }

    std::vector<DWORD> FindProcesses(const wchar_t* Name)
    {
        std::vector<DWORD> Result;
        const HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (Snapshot == INVALID_HANDLE_VALUE)
            return Result;

        PROCESSENTRY32W Entry = {};
        Entry.dwSize = sizeof(Entry);
        if (Process32FirstW(Snapshot, &Entry))
        {
            do
            {
                if (_wcsicmp(Entry.szExeFile, Name) == 0)
                    Result.push_back(Entry.th32ProcessID);
            } while (Process32NextW(Snapshot, &Entry));
        }

        CloseHandle(Snapshot);
        return Result;
    }

    bool HasVisibleWindow(DWORD ProcessId)
    {
        struct Context
        {
            DWORD ProcessId;
            bool Found;
        } Data = { ProcessId, false };

        EnumWindows([](HWND Window, LPARAM Param) -> BOOL
        {
            auto* Data = reinterpret_cast<Context*>(Param);
            DWORD Owner = 0;
            GetWindowThreadProcessId(Window, &Owner);
            if (Owner != Data->ProcessId || !IsWindowVisible(Window) || GetWindow(Window, GW_OWNER) != nullptr)
                return TRUE;

            wchar_t Title[256] = {};
            if (GetWindowTextW(Window, Title, static_cast<int>(std::size(Title))) == 0)
                return TRUE;

            Data->Found = true;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&Data));

        return Data.Found;
    }

    bool EnableDebugPrivilege()
    {
        HANDLE Token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &Token))
            return false;

        LUID Luid = {};
        if (!LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &Luid))
        {
            CloseHandle(Token);
            return false;
        }

        TOKEN_PRIVILEGES Privileges = {};
        Privileges.PrivilegeCount = 1;
        Privileges.Privileges[0].Luid = Luid;
        Privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        SetLastError(ERROR_SUCCESS);
        const BOOL Ok = AdjustTokenPrivileges(Token, FALSE, &Privileges, sizeof(Privileges), nullptr, nullptr);
        const DWORD Error = GetLastError();
        CloseHandle(Token);
        return Ok != FALSE && Error != ERROR_NOT_ALL_ASSIGNED;
    }

    std::wstring ResolveDllPath(const std::wstring& Requested, bool Explicit)
    {
        std::error_code Error;
        std::filesystem::path Path(Requested);

        if (!Explicit)
            Path = std::filesystem::path(ExecutableDirectory()) / DefaultDllName;

        if (Path.is_relative())
        {
            if (Explicit)
            {
                const std::filesystem::path BesideLoader = std::filesystem::path(ExecutableDirectory()) / Path;
                if (std::filesystem::exists(BesideLoader, Error))
                    Path = BesideLoader;
                else
                    Path = std::filesystem::absolute(Path, Error);
            }
            else
            {
                Path = std::filesystem::absolute(Path, Error);
            }
        }

        return Path.lexically_normal().wstring();
    }

    bool Inject(DWORD ProcessId, const std::wstring& DllPath)
    {
        constexpr DWORD Access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                 PROCESS_VM_WRITE | PROCESS_VM_READ;

        HANDLE Process = OpenProcess(Access, FALSE, ProcessId);
        if (Process == nullptr)
        {
            const DWORD Error = GetLastError();
            std::wcerr << L"[-] OpenProcess failed: " << Error;
            if (Error == ERROR_ACCESS_DENIED)
                std::wcerr << L" (access denied - run loader.exe as administrator)";
            std::wcerr << L"\n";
            return false;
        }

        BOOL Wow64 = FALSE;
        if (IsWow64Process(Process, &Wow64) && Wow64)
        {
            std::wcerr << L"[-] dota2.exe is 32-bit. Build and use an x64 loader.\n";
            CloseHandle(Process);
            return false;
        }

        const std::size_t Bytes = (DllPath.size() + 1) * sizeof(wchar_t);
        void* RemotePath = VirtualAllocEx(Process, nullptr, Bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (RemotePath == nullptr)
        {
            std::wcerr << L"[-] VirtualAllocEx failed: " << GetLastError() << L"\n";
            CloseHandle(Process);
            return false;
        }

        if (!WriteProcessMemory(Process, RemotePath, DllPath.c_str(), Bytes, nullptr))
        {
            std::wcerr << L"[-] WriteProcessMemory failed: " << GetLastError() << L"\n";
            VirtualFreeEx(Process, RemotePath, 0, MEM_RELEASE);
            CloseHandle(Process);
            return false;
        }

        const HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
        const auto LoadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            Kernel32 ? GetProcAddress(Kernel32, "LoadLibraryW") : nullptr);
        if (LoadLibrary == nullptr)
        {
            std::wcerr << L"[-] Could not resolve LoadLibraryW.\n";
            VirtualFreeEx(Process, RemotePath, 0, MEM_RELEASE);
            CloseHandle(Process);
            return false;
        }

        HANDLE Thread = CreateRemoteThread(Process, nullptr, 0, LoadLibrary, RemotePath, 0, nullptr);
        if (Thread == nullptr)
        {
            std::wcerr << L"[-] CreateRemoteThread failed: " << GetLastError() << L"\n";
            VirtualFreeEx(Process, RemotePath, 0, MEM_RELEASE);
            CloseHandle(Process);
            return false;
        }

        const DWORD Wait = WaitForSingleObject(Thread, 30000);
        const bool Finished = Wait == WAIT_OBJECT_0;
        DWORD ExitCode = 0;
        if (Finished)
            GetExitCodeThread(Thread, &ExitCode);

        CloseHandle(Thread);
        if (Finished)
            VirtualFreeEx(Process, RemotePath, 0, MEM_RELEASE);
        CloseHandle(Process);

        if (!Finished)
        {
            // The remote thread may still be using the path, so leave the small
            // allocation in the target process instead of risking a crash.
            std::wcerr << L"[-] Timed out while waiting for the DLL to load.\n";
            return false;
        }

        if (ExitCode == 0)
        {
            std::wcerr << L"[-] LoadLibraryW returned NULL. The DLL failed to load.\n"
                          L"    Check that anchousechanger.log has no early errors and that the DLL architecture matches the game.\n";
            return false;
        }

        std::wcout << L"[+] Injected successfully. DLL base: 0x" << std::hex << ExitCode << std::dec << L"\n";
        return true;
    }

    void PrintUsage()
    {
        std::wcout <<
            L"anchousechanger loader\n"
            L"\n"
            L"Usage:\n"
            L"  loader.exe [options] [dll_path]\n"
            L"\n"
            L"Options:\n"
            L"  --launch, -l      Start Dota 2 through Steam if it is not running.\n"
            L"  --pid <pid>, -p   Inject into a specific process instead of finding dota2.exe.\n"
            L"  --dll <path>, -d  DLL to inject (default: anchousechanger.dll beside loader.exe).\n"
            L"  --help, -h        Show this help.\n"
            L"\n"
            L"Examples:\n"
            L"  loader.exe\n"
            L"  loader.exe --launch\n"
            L"  loader.exe -p 12345\n"
            L"  loader.exe D:\\tools\\anchousechanger.dll\n";
    }

    bool ParseUnsigned(const wchar_t* Text, DWORD& Value)
    {
        wchar_t* End = nullptr;
        const unsigned long Parsed = wcstoul(Text, &End, 10);
        if (End == Text || *End != L'\0' || Parsed == 0 || Parsed > 0xFFFFFFFFul)
            return false;
        Value = static_cast<DWORD>(Parsed);
        return true;
    }
}

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"anchousechanger loader");

    bool Launch = false;
    bool ExplicitDll = false;
    DWORD ProcessId = 0;
    std::wstring DllPath;

    for (int i = 1; i < argc; ++i)
    {
        const std::wstring Arg = argv[i];
        if (Arg == L"--help" || Arg == L"-h")
        {
            PrintUsage();
            return 0;
        }
        if (Arg == L"--launch" || Arg == L"-l")
        {
            Launch = true;
            continue;
        }
        if (Arg == L"--pid" || Arg == L"-p")
        {
            if (i + 1 >= argc || !ParseUnsigned(argv[++i], ProcessId))
            {
                std::wcerr << L"[-] Invalid process id.\n";
                return 1;
            }
            continue;
        }
        if (Arg == L"--dll" || Arg == L"-d")
        {
            if (i + 1 >= argc)
            {
                std::wcerr << L"[-] Missing path after --dll.\n";
                return 1;
            }
            DllPath = argv[++i];
            ExplicitDll = true;
            continue;
        }
        if (!Arg.empty() && Arg[0] == L'-')
        {
            std::wcerr << L"[-] Unknown option: " << Arg << L"\n";
            PrintUsage();
            return 1;
        }
        if (ExplicitDll)
        {
            std::wcerr << L"[-] More than one DLL path was specified.\n";
            return 1;
        }

        DllPath = Arg;
        ExplicitDll = true;
    }

    if (DllPath.empty())
        DllPath = DefaultDllName;

    DllPath = ResolveDllPath(DllPath, ExplicitDll);
    std::error_code FileError;
    if (!std::filesystem::is_regular_file(DllPath, FileError))
    {
        std::wcerr << L"[-] DLL not found: " << DllPath << L"\n";
        return 2;
    }

    std::wcout << L"[*] DLL: " << DllPath << L"\n";

    if (ProcessId == 0)
    {
        std::vector<DWORD> Processes = FindProcesses(ProcessName);
        if (Processes.empty() && Launch)
        {
            std::wcout << L"[*] Dota 2 is not running, starting it through Steam...\n";
            const HINSTANCE Result = ShellExecuteW(nullptr, L"open", SteamLaunchUrl, nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<std::intptr_t>(Result) <= 32)
            {
                std::wcerr << L"[-] Could not start Steam. Start Dota 2 manually and run the loader again.\n";
                return 2;
            }

            const auto Deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
            while (std::chrono::steady_clock::now() < Deadline)
            {
                Sleep(500);
                Processes = FindProcesses(ProcessName);
                if (!Processes.empty())
                    break;
            }

            if (Processes.empty())
            {
                std::wcerr << L"[-] Dota 2 did not start within 3 minutes.\n";
                return 2;
            }

            std::wcout << L"[*] Waiting for the game window...\n";
            const auto WindowDeadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
            while (std::chrono::steady_clock::now() < WindowDeadline && !HasVisibleWindow(Processes.front()))
                Sleep(500);

            // Give the game time to finish loading the main menu and the GC session.
            std::wcout << L"[*] Waiting 20 seconds for the game to initialize...\n";
            Sleep(20000);
        }

        if (Processes.empty())
        {
            std::wcerr << L"[-] Dota 2 is not running. Start the game, then run loader.exe again.\n"
                          L"    Tip: use --launch to start it through Steam automatically.\n";
            return 2;
        }

        if (Processes.size() > 1)
            std::wcout << L"[!] Found " << Processes.size() << L" dota2.exe processes, using pid " << Processes.front() << L".\n";

        ProcessId = Processes.front();
    }

    std::wcout << L"[*] Target process: " << ProcessId << L"\n";
    if (!EnableDebugPrivilege())
        std::wcout << L"[!] SeDebugPrivilege is not available. If injection fails, run loader.exe as administrator.\n";

    return Inject(ProcessId, DllPath) ? 0 : 3;
}
