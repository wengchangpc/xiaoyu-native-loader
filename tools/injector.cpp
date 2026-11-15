// xiaoyu_inject.exe —— 极简 x64 远程线程注入器
//
// 用法：
//   xiaoyu_inject.exe <进程名或 PID> [DLL 路径]
//   xiaoyu_inject.exe Minecraft.Windows.exe
//   xiaoyu_inject.exe 12345 C:\Xiaoyu\xiaoyu_loader.dll
//   xiaoyu_inject.exe Minecraft.Windows.exe --list     // 只列出匹配的进程
//   xiaoyu_inject.exe Minecraft.Windows.exe --free     // 卸载（FreeLibrary）
//
// 不传 DLL 路径时，自动用本 exe 同目录下的 xiaoyu_loader.dll。
// 输出全 ASCII —— 免得在 GBK 控制台里变乱码。

#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct ProcEntry {
    DWORD pid;
    DWORD ppid;
    std::wstring name;
};

std::wstring ToLower(std::wstring s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] >= L'A' && s[i] <= L'Z') s[i] = static_cast<wchar_t>(s[i] - L'A' + L'a');
    }
    return s;
}

bool EndsWithNoCase(const std::wstring& s, const std::wstring& suffix) {
    if (s.size() < suffix.size()) return false;
    return ToLower(s.substr(s.size() - suffix.size())) == ToLower(suffix);
}

std::vector<ProcEntry> SnapshotProcesses() {
    std::vector<ProcEntry> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcEntry e;
            e.pid = pe.th32ProcessID;
            e.ppid = pe.th32ParentProcessID;
            e.name = pe.szExeFile;
            out.push_back(e);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

// 把 "Minecraft.Windows.exe" 或 "minecraft" 都当成同一个目标
std::vector<ProcEntry> MatchTarget(const std::vector<ProcEntry>& all, const std::wstring& want) {
    std::vector<ProcEntry> hit;
    std::wstring lower = ToLower(want);
    bool wantHasExt = EndsWithNoCase(lower, L".exe");
    for (size_t i = 0; i < all.size(); ++i) {
        std::wstring name = ToLower(all[i].name);
        bool ok = false;
        if (wantHasExt) {
            ok = (name == lower);
        } else {
            ok = (name == lower + L".exe") || (name == lower);
        }
        if (ok) hit.push_back(all[i]);
    }
    return hit;
}

// 目标进程是 32 位的话，直接判定"别注了"，省得崩游戏
const char* ArchOf(HANDLE proc) {
    typedef BOOL(WINAPI * fn_IsWow64Process2)(HANDLE, USHORT*, USHORT*);
    static fn_IsWow64Process2 p2 = nullptr;
    static bool resolved = false;
    if (!resolved) {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (k32 != nullptr) {
            p2 = reinterpret_cast<fn_IsWow64Process2>(GetProcAddress(k32, "IsWow64Process2"));
        }
        resolved = true;
    }
    if (p2 != nullptr) {
        USHORT processMachine = 0, nativeMachine = 0;
        if (p2(proc, &processMachine, &nativeMachine)) {
            if (processMachine == IMAGE_FILE_MACHINE_UNKNOWN) return "x64-native";
            if (processMachine == IMAGE_FILE_MACHINE_I386) return "x86-wow64";
            if (processMachine == IMAGE_FILE_MACHINE_ARM64) return "arm64";
            return "other";
        }
    }
    BOOL wow = FALSE;
    if (IsWow64Process(proc, &wow)) return wow ? "x86-wow64" : "x64-native";
    return "unknown";
}

bool InjectDll(DWORD pid, const std::wstring& dllPath, bool doFree) {
    const DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                         PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
                         SYNCHRONIZE;

    HANDLE proc = OpenProcess(access, FALSE, pid);
    if (proc == nullptr) {
        printf("[FAIL] OpenProcess(pid=%lu) err=%lu -- run as administrator?\n",
               static_cast<unsigned long>(pid), static_cast<unsigned long>(GetLastError()));
        return false;
    }

    printf("[info] target pid   : %lu\n", static_cast<unsigned long>(pid));
    printf("[info] target arch  : %s\n", ArchOf(proc));

    // DLL 存在性 + 位数检查（注入 32 位进程必崩）
    HANDLE f = CreateFileW(dllPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        printf("[FAIL] cannot open dll: %ls (err=%lu)\n", dllPath.c_str(),
               static_cast<unsigned long>(GetLastError()));
        CloseHandle(proc);
        return false;
    }
    DWORD fsize = GetFileSize(f, nullptr);
    CloseHandle(f);
    printf("[info] dll          : %ls (%lu bytes)\n", dllPath.c_str(),
           static_cast<unsigned long>(fsize));

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (remote == nullptr) {
        printf("[FAIL] VirtualAllocEx err=%lu\n", static_cast<unsigned long>(GetLastError()));
        CloseHandle(proc);
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, &written) || written != bytes) {
        printf("[FAIL] WriteProcessMemory err=%lu\n", static_cast<unsigned long>(GetLastError()));
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }
    printf("[info] path buffer  : 0x%p (%lu bytes written)\n", remote,
           static_cast<unsigned long>(written));

    const char* procName = doFree ? "FreeLibraryW" : "LoadLibraryW";
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE start = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(k32, procName));
    if (start == nullptr) {
        printf("[FAIL] GetProcAddress(%s)\n", procName);
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HANDLE th = CreateRemoteThread(proc, nullptr, 0, start, remote, 0, nullptr);
    if (th == nullptr) {
        printf("[FAIL] CreateRemoteThread err=%lu\n", static_cast<unsigned long>(GetLastError()));
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    printf("[info] waiting for %s ...\n", procName);
    DWORD wait = WaitForSingleObject(th, 30000);
    DWORD ret = 0;
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeThread(th, &ret);
        if (!doFree) {
            if (ret == 0) {
                printf("[FAIL] LoadLibraryW returned NULL -- DllMain returned FALSE, or the DLL "
                       "failed to map (bad arch / missing dependency).\n");
                printf("       Check the game's own log and C:\\Xiaoyu\\loader.log\n");
            } else {
                printf("[OK]   LoadLibraryW -> HMODULE 0x%lX\n", static_cast<unsigned long>(ret));
            }
        } else {
            printf("[OK]   FreeLibraryW ret=%lu\n", static_cast<unsigned long>(ret));
        }
    } else {
        printf("[WARN] thread wait timeout (code=%lu) -- DLL may still be loading\n",
               static_cast<unsigned long>(wait));
    }

    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return wait == WAIT_OBJECT_0 && (doFree || ret != 0);
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (n == 0) return L".";
    std::wstring s(buf, n);
    size_t pos = s.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    return s.substr(0, pos);
}

void Usage() {
    printf("xiaoyu_inject -- minimal x64 DLL injector\n\n");
    printf("usage:\n");
    printf("  xiaoyu_inject.exe <process-name|pid> [dll-path] [--list|--free]\n\n");
    printf("examples:\n");
    printf("  xiaoyu_inject.exe Minecraft.Windows.exe\n");
    printf("  xiaoyu_inject.exe Minecraft.Windows.exe --list\n");
    printf("  xiaoyu_inject.exe 1234 C:\\Xiaoyu\\xiaoyu_loader.dll\n\n");
    printf("no dll-path given -> uses xiaoyu_loader.dll next to this exe\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    printf("=== xiaoyu_inject (x64) ===\n");

    if (argc < 2) {
        Usage();
        return 2;
    }

    std::wstring target = argv[1];
    std::wstring dllPath;
    bool doFree = false;
    bool listOnly = false;

    for (int i = 2; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--list") {
            listOnly = true;
        } else if (a == L"--free") {
            doFree = true;
        } else if (a == L"--help" || a == L"-h") {
            Usage();
            return 0;
        } else {
            dllPath = a;
        }
    }

    if (dllPath.empty()) {
        dllPath = ExeDir() + L"\\xiaoyu_loader.dll";
    }

    // 目标既可能是 PID 也可能是名字
    bool numeric = !target.empty();
    for (size_t i = 0; i < target.size(); ++i) {
        if (target[i] < L'0' || target[i] > L'9') {
            numeric = false;
            break;
        }
    }

    std::vector<ProcEntry> procs = SnapshotProcesses();
    std::vector<DWORD> pids;

    if (numeric && target.size() <= 10) {
        // 纯数字：先当 PID 用；找不到再退回按名字匹配（以防进程名就叫纯数字）
        DWORD pid = static_cast<DWORD>(_wtoi(target.c_str()));
        bool exists = false;
        for (size_t i = 0; i < procs.size(); ++i) {
            if (procs[i].pid == pid) {
                exists = true;
                break;
            }
        }
        if (exists) {
            pids.push_back(pid);
        } else {
            std::vector<ProcEntry> hits = MatchTarget(procs, target);
            for (size_t i = 0; i < hits.size(); ++i) pids.push_back(hits[i].pid);
        }
    } else {
        std::vector<ProcEntry> hits = MatchTarget(procs, target);
        printf("[info] matching '%ls' -> %d process(es)\n", target.c_str(),
               static_cast<int>(hits.size()));
        for (size_t i = 0; i < hits.size(); ++i) {
            printf("       pid %-7lu ppid %-7lu %ls\n", static_cast<unsigned long>(hits[i].pid),
                   static_cast<unsigned long>(hits[i].ppid), hits[i].name.c_str());
            pids.push_back(hits[i].pid);
        }
    }

    if (pids.empty()) {
        printf("[FAIL] no process named '%ls' is running.\n", target.c_str());
        printf("       Start the game first, and pass the image name as it appears in Task "
               "Manager (e.g. Minecraft.Windows.exe).\n");
        return 1;
    }
    if (pids.size() > 1) {
        printf("[FAIL] %d candidates -- pass an explicit pid instead:\n",
               static_cast<int>(pids.size()));
        for (size_t i = 0; i < pids.size(); ++i) {
            printf("       xiaoyu_inject.exe %lu\n", static_cast<unsigned long>(pids[i]));
        }
        return 1;
    }
    if (listOnly) return 0;

    bool ok = InjectDll(pids[0], dllPath, doFree);
    if (ok) {
        printf("\n[done] now check the logs:\n");
        printf("       C:\\Xiaoyu\\loader.log   (loader side)\n");
        printf("       C:\\Xiaoyu\\probe.log    (python probe result)\n");
    }
    return ok ? 0 : 1;
}
