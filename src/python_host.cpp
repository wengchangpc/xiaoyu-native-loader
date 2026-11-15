// python_host.cpp
#include "python_host.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "log.h"

typedef int (*fn_is_initialized)(void);
typedef int (*fn_gil_ensure)(void);
typedef void (*fn_gil_release)(int);
typedef int (*fn_run_simple)(const char*);
typedef int (*fn_run_simple_flags)(const char*, int);
typedef const char* (*fn_get_version)(void);

static std::string Lower(const std::string& s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });
    return out;
}

static std::string Narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                   nullptr, 0, nullptr, nullptr);
    std::string out((size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], need,
                        nullptr, nullptr);
    return out;
}

struct ModuleEntry {
    std::wstring name;
    HMODULE handle = nullptr;
    size_t base = 0;
    size_t size = 0;
};

static std::vector<ModuleEntry> SnapshotModules() {
    std::vector<ModuleEntry> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                           GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return out;
    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            ModuleEntry e;
            e.name = me.szModule;
            e.handle = me.hModule;
            e.base = (size_t)me.modBaseAddr;
            e.size = (size_t)me.modBaseSize;
            out.push_back(e);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return out;
}

static bool LooksLikePython(const std::string& lower) {
    if (lower.size() < 10) return false;
    if (lower.compare(0, 6, "python") != 0) return false;
    return lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".dll") == 0;
}

bool PyFindModule(PyApi* api) {
    if (api == nullptr) return false;
    if (api->mod != nullptr) return true;

    std::vector<ModuleEntry> mods = SnapshotModules();
    // 先找带版本号的（python27.dll / python311.dll），再退 python3.dll
    for (const ModuleEntry& m : mods) {
        std::string lower = Lower(Narrow(m.name));
        if (!LooksLikePython(lower)) continue;
        if (lower == "python3.dll") continue;  // 3.x 的转发 dll，导出不全
        api->mod = m.handle;
        api->module_name = m.name;
        return true;
    }
    for (const ModuleEntry& m : mods) {
        std::string lower = Lower(Narrow(m.name));
        if (lower == "python3.dll") {
            api->mod = m.handle;
            api->module_name = m.name;
            return true;
        }
    }
    return false;
}

bool PyResolve(PyApi* api) {
    if (api == nullptr) return false;
    if (api->ok) return true;
    if (!PyFindModule(api)) return false;

    api->p_is_initialized = (void*)GetProcAddress(api->mod, "Py_IsInitialized");
    api->p_gil_ensure = (void*)GetProcAddress(api->mod, "PyGILState_Ensure");
    api->p_gil_release = (void*)GetProcAddress(api->mod, "PyGILState_Release");
    api->p_run_simple = (void*)GetProcAddress(api->mod, "PyRun_SimpleString");
    api->p_run_simple_flags =
        (void*)GetProcAddress(api->mod, "PyRun_SimpleStringFlags");
    api->p_get_version = (void*)GetProcAddress(api->mod, "Py_GetVersion");

    bool has_run = (api->p_run_simple != nullptr) ||
                   (api->p_run_simple_flags != nullptr);
    api->ok = (api->p_is_initialized != nullptr) &&
              (api->p_gil_ensure != nullptr) &&
              (api->p_gil_release != nullptr) && has_run;

    LogLine("[py] module=%s ok=%d run=%d flags=%d gil=%d",
            Narrow(api->module_name).c_str(), api->ok ? 1 : 0,
            api->p_run_simple ? 1 : 0, api->p_run_simple_flags ? 1 : 0,
            api->p_gil_ensure ? 1 : 0);
    return api->ok;
}

bool PyIsReady(PyApi* api) {
    if (api == nullptr || api->p_is_initialized == nullptr) return false;
    return ((fn_is_initialized)api->p_is_initialized)() != 0;
}

std::string PyVersion(PyApi* api) {
    if (api == nullptr || api->p_get_version == nullptr) return std::string();
    const char* v = ((fn_get_version)api->p_get_version)();
    return v ? std::string(v) : std::string();
}

int PyRunScript(PyApi* api, const char* code_utf8) {
    if (api == nullptr || !api->ok || code_utf8 == nullptr) return -1;
    if (!PyIsReady(api)) return -2;

    int state = ((fn_gil_ensure)api->p_gil_ensure)();
    int rc = 0;
    if (api->p_run_simple != nullptr) {
        rc = ((fn_run_simple)api->p_run_simple)(code_utf8);
    } else {
        rc = ((fn_run_simple_flags)api->p_run_simple_flags)(code_utf8, 0);
    }
    ((fn_gil_release)api->p_gil_release)(state);
    return rc;
}

void DumpLoadedModules() {
    std::vector<ModuleEntry> mods = SnapshotModules();
    LogLine("[mod] 共 %d 个已加载模块", (int)mods.size());
    for (const ModuleEntry& m : mods) {
        std::string name = Narrow(m.name);
        std::string lower = Lower(name);
        bool interesting =
            LooksLikePython(lower) || lower.find("netease") != std::string::npos ||
            lower.find("pynet") != std::string::npos ||
            lower.find("minecraft") != std::string::npos ||
            lower.find("modsdk") != std::string::npos;
        LogLine("[mod] %s%s  base=0x%08llX size=0x%llX", interesting ? "* " : "  ",
                name.c_str(), (unsigned long long)m.base,
                (unsigned long long)m.size);
    }
}
