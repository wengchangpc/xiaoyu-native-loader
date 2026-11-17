// probe.cpp - 进程内自诊断：RTTI -> COL -> vtable -> 堆上活对象
//
// 这条链的每一步都有硬性自校验，任何一步对不上就放弃这个类，
// 绝不"猜一个差不多的地址"往下走 —— 内存模块最怕的就是偏移错一位，
// 读出来是垃圾数据，表现成"功能开了没反应"或者直接崩。
//
// MSVC x64 的 RTTI 布局：
//
//   TypeDescriptor {
//       void* pVFTable;      // +0
//       void* spare;         // +8
//       char  name[];        // +16   ".?AVClientInstance@@"
//   }
//
//   RTTICompleteObjectLocator {
//       uint32 signature;        // +0   必须 == 1
//       uint32 offset;           // +4
//       uint32 cdOffset;         // +8
//       uint32 pTypeDescriptor;  // +12  ★RVA，不是绝对地址
//       uint32 pClassDescriptor; // +16  RVA
//       uint32 pSelf;            // +20  RVA，指向自身 —— 最强的自校验
//   }
//
//   vtable[-1] = &COL            // ★绝对地址，重定位后
//   vtable[0]  = 第一个虚函数
//
// RVA 和绝对地址混用是这类代码最常见的错，两边都要按各自的形式算。
#include "probe.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "log.h"
#include "memory.h"

namespace xiaoyu {
namespace probe {
namespace {

// ── 诊断日志（单独一个文件，不和 native.log 混） ──────────────
FILE* g_pf = nullptr;

void POpen() {
    if (g_pf != nullptr) return;
    CreateDirectoryW(L"C:\\Xiaoyu", nullptr);
    CreateDirectoryW(L"C:\\Xiaoyu\\dump", nullptr);
    // 二进制写 + 自己控制编码：源文件是 /utf-8，所以窄字符串字面量本身
    // 就是 UTF-8 字节，直接 fwrite 出去就是合法的 UTF-8 文件。
    // 用 "w, ccs=UTF-8" 反而会和 fputs 的多字节写入打架。
    g_pf = fopen("C:\\Xiaoyu\\dump\\probe.log", "wb");
    if (g_pf == nullptr) {
        g_pf = fopen("probe.log", "wb");
    }
}

// 宽字符串转 UTF-8，给 %s 用
const char* W2U(const wchar_t* w, char* buf, int cap) {
    if (w == nullptr || buf == nullptr || cap <= 0) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, cap - 1, nullptr, nullptr);
    if (n <= 0) {
        buf[0] = 0;
        return buf;
    }
    buf[n] = 0;
    return buf;
}

void P(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_pf != nullptr) {
        fputs(buf, g_pf);
        fputc('\n', g_pf);
        fflush(g_pf);
    }
    LogLine("%s", buf);
}

void PClose() {
    if (g_pf != nullptr) {
        fclose(g_pf);
        g_pf = nullptr;
    }
}

// ── 目标类 ──────────────────────────────────────────────────────
// 顺序有意义：ClientInstance 是根，先把它拿到，后面的都能从它的字段读出来。
const char* kClasses[] = {
    "ClientInstance",
    "LocalPlayer",
    "Player",
    "Actor",
    "Mob",
    "Level",
    "BlockSource",
    "Dimension",
    "ItemStack",
    "GameMode",
    "Inventory",
    "Container",
};
const int kClassCount = (int)(sizeof(kClasses) / sizeof(kClasses[0]));

struct ClassResult {
    const char* name;
    uintptr_t type_desc;
    uintptr_t col;
    uintptr_t vtable;
    bool chain_ok;
    int instance_count;
    uintptr_t instances[8];
};

ClassResult g_results[kClassCount];

// ── 链条各步 ────────────────────────────────────────────────────

// 找 RTTI 名字，返回 TypeDescriptor 起点。
bool FindTypeDescriptor(const char* cls, uintptr_t* td_out) {
    char pat[128];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, ".?AV%s@@", cls);
    const uintptr_t name = mem::ScanText(pat);
    if (name == 0) return false;
    // 名字前 16 字节是 pVFTable + spare；再往前就越界了，做个下限保护
    if (name < mem::Base() + 0x1000 + 16) return false;
    *td_out = name - 16;

    // 顺手校验一下 pVFTable 像个代码指针（在映像内）
    uintptr_t vft = 0;
    if (!mem::Read(*td_out, &vft, sizeof(vft))) return false;
    if (!mem::InImage(vft)) {
        P("    ! %s: pVFTable 0x%p 不在映像内，TypeDescriptor 可能算错了",
          cls, (void*)vft);
        // 不直接否决 —— 有些情况 type_info 的 vftable 在只读段之外
    }
    return true;
}

// 找所有 pTypeDescriptor == td 的 COL，挑第一个自校验通过的。
bool FindCol(uintptr_t td, uintptr_t* col_out) {
    const uintptr_t base = mem::Base();
    const uint32_t td_rva = (uint32_t)(td - base);

    uintptr_t from = 0;
    int tried = 0;
    for (int guard = 0; guard < 16; ++guard) {
        const uintptr_t ptd_pos = mem::ScanDword(td_rva, from);
        if (ptd_pos == 0) break;
        from = ptd_pos + 1;
        if (ptd_pos < 12) continue;
        const uintptr_t col = ptd_pos - 12;
        ++tried;

        uint32_t sig = 0, ptd = 0, pself = 0;
        if (!mem::Read(col, &sig, 4)) continue;
        if (sig != 1) continue;
        if (!mem::Read(col + 12, &ptd, 4)) continue;
        if (ptd != td_rva) continue;
        if (!mem::Read(col + 20, &pself, 4)) continue;
        if (pself != (uint32_t)(col - base)) continue;   // 指向自身，强校验

        *col_out = col;
        return true;
    }
    if (tried > 0) {
        P("    ! COL 候选 %d 个，自校验都没过", tried);
    }
    return false;
}

// vtable[-1] 存 COL 绝对地址。
bool FindVtable(uintptr_t col, uintptr_t* vt_out) {
    uintptr_t hits[16];
    const int n = mem::FindAllQword((uint64_t)col, hits, 16);
    if (n <= 0) return false;
    if (n > 1) {
        P("    ~ COL 被 %d 处引用（多重继承/vtable 副本），取第一处", n);
    }
    *vt_out = hits[0] + 8;
    return true;
}

// 走完一个类的链条
void ResolveClass(ClassResult* r) {
    r->type_desc = 0;
    r->col = 0;
    r->vtable = 0;
    r->chain_ok = false;
    r->instance_count = 0;

    if (!FindTypeDescriptor(r->name, &r->type_desc)) {
        P("  %-16s RTTI 名字没找到", r->name);
        return;
    }
    if (!FindCol(r->type_desc, &r->col)) {
        P("  %-16s td=0x%p 但没定位到 COL", r->name, (void*)r->type_desc);
        return;
    }
    if (!FindVtable(r->col, &r->vtable)) {
        P("  %-16s col=0x%p 但没找到 vtable", r->name, (void*)r->col);
        return;
    }
    r->chain_ok = true;
    P("  %-16s td=0x%p col=0x%p vtable=0x%p",
      r->name, (void*)r->type_desc, (void*)r->col, (void*)r->vtable);
}

// ── 扫堆找实例 ──────────────────────────────────────────────────
void FindInstances(ClassResult* results, int count) {
    uint64_t needles[16];
    int map[16];
    int nc = 0;
    for (int i = 0; i < count && nc < 16; ++i) {
        if (!results[i].chain_ok) continue;
        needles[nc] = (uint64_t)results[i].vtable;
        map[nc] = i;
        ++nc;
    }
    if (nc == 0) {
        P("\n没有可用的 vtable，跳过堆扫描");
        return;
    }

    P("\n扫描进程私有内存（%d 个 vtable 一起搜）...", nc);
    const int kMaxHits = 512;
    static mem::HeapHit hits[kMaxHits];
    size_t scanned = 0;
    const ULONGLONG t0 = GetTickCount64();
    const int n = mem::ScanHeapQword(needles, nc, hits, kMaxHits, &scanned);
    const ULONGLONG dt = GetTickCount64() - t0;

    P("扫了 %.1fMB，%llums，命中 %d 处", (double)scanned / 1048576.0,
      (unsigned long long)dt, n);

    for (int i = 0; i < n; ++i) {
        ClassResult& r = results[map[hits[i].index]];
        if (r.instance_count < 8) {
            r.instances[r.instance_count++] = hits[i].addr;
        }
    }

    P("");
    for (int i = 0; i < count; ++i) {
        ClassResult& r = results[i];
        if (!r.chain_ok) continue;
        P("%-16s %d 个实例", r.name, r.instance_count);
        for (int k = 0; k < r.instance_count; ++k) {
            const uintptr_t a = r.instances[k];
            // 打印前 16 个 qword，人工看字段像不像指针
            uint64_t f[16] = {0};
            if (!mem::Read(a, f, sizeof(f))) continue;
            char line[512];
            int pos = 0;
            for (int j = 0; j < 16 && pos < (int)sizeof(line) - 24; ++j) {
                if (f[j] > 0x10000) {
                    pos += _snprintf_s(line + pos, sizeof(line) - pos, _TRUNCATE,
                                       "%llX ", (unsigned long long)f[j]);
                } else {
                    pos += _snprintf_s(line + pos, sizeof(line) - pos, _TRUNCATE,
                                       ". ");
                }
            }
            P("   %016llX  [%s]", (unsigned long long)a, line);
        }
    }
}

// ── ClientInstance 字段探测 ─────────────────────────────────────
// 拿到 ClientInstance* 之后，最想知道的是「哪个偏移是 LocalPlayer」。
// 做法：把对象前 0x400 字节当 qword 数组扫一遍，凡是值落在堆上、
// 且该地址首字段是某个已知 vtable 的，就报出来。
void ProbeClientInstanceFields(ClassResult* results, int count) {
    const ClassResult* ci = nullptr;
    for (int i = 0; i < count; ++i) {
        if (results[i].chain_ok && results[i].instance_count > 0 &&
            strcmp(results[i].name, "ClientInstance") == 0) {
            ci = &results[i];
            break;
        }
    }
    if (ci == nullptr) {
        P("\n没有 ClientInstance 实例，跳过字段探测");
        return;
    }

    const uintptr_t inst = ci->instances[0];
    P("\n=== ClientInstance @ 0x%p 前 0x400 字节里的已知对象指针 ===",
      (void*)inst);

    int hits = 0;
    for (int off = 8; off < 0x400; off += 8) {
        uintptr_t p = 0;
        if (!mem::Read(inst + off, &p, sizeof(p))) break;
        if (p < 0x10000) continue;
        // 看这个指针指向的对象首字段是不是某个已知 vtable
        uintptr_t vt = 0;
        if (!mem::Read(p, &vt, sizeof(vt))) continue;
        for (int i = 0; i < count; ++i) {
            if (!results[i].chain_ok) continue;
            if (vt == results[i].vtable) {
                P("  +0x%03X -> 0x%p  (%s)", off, (void*)p, results[i].name);
                ++hits;
                break;
            }
        }
    }
    if (hits == 0) {
        P("  （没有直接命中，说明字段里存的是间接引用或尚未初始化）");
    }
}

}  // namespace

// ── 对外接口 ────────────────────────────────────────────────────
void Run() {
    POpen();
    const ULONGLONG t_all = GetTickCount64();

    P("========== xiaoyu probe ==========");
    HMODULE mod = GetModuleHandleW(nullptr);
    wchar_t exe_w[MAX_PATH] = {0};
    GetModuleFileNameW(mod, exe_w, MAX_PATH);
    char exe_u[MAX_PATH * 3] = {0};
    P("exe   : %s", W2U(exe_w, exe_u, sizeof(exe_u)));
    P("pid   : %lu", (unsigned long)GetCurrentProcessId());

    if (!mem::Init()) {
        P("!! mem::Init 失败，后续全部跳过");
        PClose();
        return;
    }
    P("base  : 0x%p  image=%.1fMB  regions=%d",
      (void*)mem::Base(), (double)mem::ImageSize() / 1048576.0,
      (int)mem::RegionCount());

    P("\n=== RTTI 链条 ===");
    for (int i = 0; i < kClassCount; ++i) {
        g_results[i].name = kClasses[i];
        ResolveClass(&g_results[i]);
    }

    FindInstances(g_results, kClassCount);
    ProbeClientInstanceFields(g_results, kClassCount);

    // 顺手把主模块 dump 一份 —— 进程内 dump 不经过 ReadProcessMemory，
    // 反作弊看不到，也就不会像外部扫描那样被中途打断。
    P("\n=== dump 主模块 ===");
    if (mem::DumpImage(L"C:\\Xiaoyu\\dump\\main.selfdump.exe")) {
        P("已写入 C:\\Xiaoyu\\dump\\main.selfdump.exe");
    } else {
        P("dump 失败");
    }

    P("\n总耗时 %llums", (unsigned long long)(GetTickCount64() - t_all));
    P("========== probe done ==========");
    PClose();
}

namespace {
DWORD WINAPI ProbeThread(LPVOID param) {
    const unsigned delay_ms = (unsigned)(uintptr_t)param;
    if (delay_ms > 0) Sleep(delay_ms);
    Run();
    return 0;
}
}  // namespace

void Start(unsigned delay_ms) {
    HANDLE th = CreateThread(nullptr, 0, ProbeThread, (LPVOID)(uintptr_t)delay_ms, 0,
                             nullptr);
    if (th != nullptr) {
        CloseHandle(th);
    } else {
        LogLine("[probe] CreateThread failed err=%lu", (unsigned long)GetLastError());
    }
}

int ClientInstanceCount() {
    for (int i = 0; i < kClassCount; ++i) {
        if (strcmp(g_results[i].name, "ClientInstance") == 0) {
            return g_results[i].instance_count;
        }
    }
    return 0;
}

uintptr_t FirstClientInstance() {
    for (int i = 0; i < kClassCount; ++i) {
        if (strcmp(g_results[i].name, "ClientInstance") == 0) {
            return g_results[i].instance_count > 0 ? g_results[i].instances[0] : 0;
        }
    }
    return 0;
}

}  // namespace probe
}  // namespace xiaoyu
