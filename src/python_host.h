// python_host.h - 在宿主进程里找 Python 解释器并执行脚本
#pragma once
#include <windows.h>
#include <string>

struct PyApi {
    HMODULE mod = nullptr;
    std::wstring module_name;
    void* p_is_initialized = nullptr;
    void* p_gil_ensure = nullptr;
    void* p_gil_release = nullptr;
    void* p_run_simple = nullptr;
    void* p_run_simple_flags = nullptr;
    void* p_get_version = nullptr;
    bool ok = false;
};

// 在已加载模块里找 python*.dll（找不到返回 false）
bool PyFindModule(PyApi* api);

// 解析导出函数（找不到模块返回 false；缺关键导出返回 false）
bool PyResolve(PyApi* api);

// 解释器是否已初始化
bool PyIsReady(PyApi* api);

// 解释器版本串（可能为空）
std::string PyVersion(PyApi* api);

// 在持有 GIL 的前提下执行 UTF-8 脚本，返回 0 表示脚本无异常
int PyRunScript(PyApi* api, const char* code_utf8);

// 把当前进程已加载模块打进日志（探针用）
void DumpLoadedModules();
