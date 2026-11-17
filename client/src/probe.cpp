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
#include <cmath>
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

// ── 扫堆找单个类的实例 ──────────────────────────────────────────
// 只搜一个 vtable 比一次搜 12 个快一个数量级（每个 needle 都要独立过一遍
// 1.2GB），所以"等 LocalPlayer 出现"这种轮询用这个，全量枚举才用 FindInstances。
int FindOne(const ClassResult* r, uintptr_t* out, int max) {
    if (!r->chain_ok || out == nullptr || max <= 0) return 0;
    const uint64_t needle = (uint64_t)r->vtable;
    static mem::HeapHit hits[256];
    size_t scanned = 0;
    const int n = mem::ScanHeapQword(&needle, 1, hits, 256, &scanned);
    int k = 0;
    for (int i = 0; i < n && k < max; ++i) {
        out[k++] = hits[i].addr;
    }
    return k;
}

// ── 扫堆找实例 ──────────────────────────────────────────────────
void FindInstances(ClassResult* results, int count) {
    // 只扫这 4 个。其余 8 个类（Player/Actor/Mob/BlockSource/ItemStack/…）
    // 都是继承链上的基类或叶子类型：对象的 +0 只会放**最派生类**的 vtable，
    // 所以扫它们的 vtable 基本只会扫到栈上的临时值和 vtable 副本，
    // 白白拖慢扫描还挤占命中名额。实测 12 个一起扫要 30 秒且命中正好
    // 打满 2048，改成 4 个之后快了三倍。
    static const char* kKey[] = {"ClientInstance", "LocalPlayer", "Level", "Dimension"};

    uint64_t needles[16];
    int map[16];
    int nc = 0;
    for (int i = 0; i < count && nc < 16; ++i) {
        if (!results[i].chain_ok) continue;
        bool key = false;
        for (int k = 0; k < 4; ++k) {
            if (strcmp(results[i].name, kKey[k]) == 0) { key = true; break; }
        }
        if (!key) continue;
        needles[nc] = (uint64_t)results[i].vtable;
        map[nc] = i;
        ++nc;
    }
    if (nc == 0) {
        P("\n没有可用的 vtable，跳过堆扫描");
        return;
    }

    P("\n扫描进程私有内存（%d 个 vtable 一起搜）...", nc);
    // 512 在假阳性多的时候会被打满（上一次实测正好 512，后面的类全被截断），
    // 提到 2048 留足余量；每个 HeapHit 16 字节，也就 32KB。
    const int kMaxHits = 2048;
    static mem::HeapHit hits[kMaxHits];
    size_t scanned = 0;

    // 全量重扫，之前轮询阶段记下的地址作废
    for (int i = 0; i < count; ++i) {
        if (results[i].chain_ok) results[i].instance_count = 0;
    }

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

// ── 对象布局分析 ────────────────────────────────────────────────
// Bedrock 的 getPos()/getRot() 是**内联**函数（不在 vtable 里），所以没法
// 从虚表反推字段偏移。改用数据特征：
//
//   1) 玩家的碰撞箱宽 0.6f、高 1.8f 是写死的常量，在 Actor 里一定存在。
//      找到它们的位置，就说明已经进入"Actor 的物理字段区"，
//      pos 通常紧挨着这一片。
//   2) 位置是三个连续的 float：|x|,|z| < 3e7（世界边界），y 在 -80~400 之间。
//      同时排除 NaN/Inf 和全零。
//
// 两条线索交叉验证，比单独猜偏移可靠得多。假阳性会有，但把范围从
// "整个对象"缩到"几十个候选"之后，人一眼就能认出来。
bool PlausibleCoord(float x, float y, float z) {
    if (!(x == x) || !(y == y) || !(z == z)) return false;   // NaN
    const float kInf = 3.0e38f;
    if (x > kInf || x < -kInf || y > kInf || y < -kInf || z > kInf || z < -kInf) {
        return false;
    }
    if (y < -80.0f || y > 400.0f) return false;
    if (x > 3.0e7f || x < -3.0e7f) return false;
    if (z > 3.0e7f || z < -3.0e7f) return false;
    return true;
}

void AnalyzeObject(const char* tag, uintptr_t inst, uintptr_t vt, int count,
                   ClassResult* results) {
    P("\n=== %s @ 0x%p 布局分析 ===", tag, (void*)inst);

    // ① 已知常量锚点
    P("  -- 常量锚点 --");
    int anchors = 0;
    for (int off = 0; off < 0x900; off += 4) {
        float f = 0.0f;
        if (!mem::Read(inst + off, &f, 4)) break;
        if (f > 0.5999f && f < 0.6001f) {
            P("     +0x%03X = 0.6f   （碰撞箱宽度）", off);
            ++anchors;
        } else if (f > 1.7999f && f < 1.8001f) {
            P("     +0x%03X = 1.8f   （碰撞箱高度）", off);
            ++anchors;
        }
    }
    if (anchors == 0) P("     （没找到，可能这个版本的宽高不是 0.6/1.8）");

    // ② 坐标三元组
    P("  -- 疑似坐标三元组 --");
    int coords = 0;
    for (int off = 0; off < 0x900; off += 4) {
        float xyz[3] = {0, 0, 0};
        if (!mem::Read(inst + off, xyz, sizeof(xyz))) break;
        if (!PlausibleCoord(xyz[0], xyz[1], xyz[2])) continue;
        // 三轴都接近 0 的几乎都是未初始化区域，真实位置很少正好在原点
        if (fabsf(xyz[0]) < 0.01f && fabsf(xyz[1]) < 0.01f && fabsf(xyz[2]) < 0.01f) {
            continue;
        }
        P("     +0x%03X  (%.3f, %.3f, %.3f)", off, xyz[0], xyz[1], xyz[2]);
        if (++coords >= 24) {
            P("     ...（后面还有很多，先只列 24 个）");
            break;
        }
    }
    if (coords == 0) P("     （没有像坐标的三元组）");

    // ③ 指向其它已知对象的指针（跨对象关系，最有价值）
    P("  -- 指向其它已知对象的指针 --");
    int ptrs = 0;
    for (int off = 8; off < 0x900; off += 8) {
        uintptr_t p = 0;
        if (!mem::Read(inst + off, &p, sizeof(p))) break;
        if (p < 0x10000) continue;
        uintptr_t pvt = 0;
        if (!mem::Read(p, &pvt, sizeof(pvt))) continue;
        for (int i = 0; i < count; ++i) {
            if (!results[i].chain_ok) continue;
            if (pvt == results[i].vtable && results[i].vtable != vt) {
                P("     +0x%03X -> 0x%p  (%s)", off, (void*)p, results[i].name);
                ++ptrs;
                break;
            }
        }
    }
    if (ptrs == 0) P("     （没有直接指向已知对象的字段）");

    // ④ 所有堆指针 + 它们指向对象的 vtable（不认识也照打）
    //    枚举实体列表时要找的是容器，而容器的类型这里不一定有 RTTI，
    //    所以不能只看"已知类型"，得把所有堆指针摊开看。
    P("  -- 所有堆指针（含未知类型，标出对方 vtable）--");
    int heap_ptrs = 0;
    for (int off = 8; off < 0x900; off += 8) {
        uintptr_t p = 0;
        if (!mem::Read(inst + off, &p, sizeof(p))) break;
        if (p < 0x100000000ULL || p > 0x7FFF00000000ULL) continue;   // 只要堆
        uintptr_t pvt = 0;
        if (!mem::Read(p, &pvt, sizeof(pvt))) continue;
        P("     +0x%03X -> 0x%p   [vtable 0x%p]", off, (void*)p, (void*)pvt);
        if (++heap_ptrs >= 40) {
            P("     ...（还有更多，先列 40 个）");
            break;
        }
    }
    if (heap_ptrs == 0) P("     （没有堆指针）");

    // ⑤ 容器识别：std::vector 是 begin/end/capacity 三个连续指针
    //    （end-end 是元素数，capacity-end 是剩余空间）。三个值单调不减、
    //    且都指向同一片堆内存，就是 vector；这通常是实体列表。
    P("  -- 容器候选（连续三个递增指针，像 std::vector）--");
    int vecs = 0;
    for (int off = 8; off + 24 <= 0x900; off += 8) {
        uintptr_t b = 0, e = 0, cap = 0;
        if (!mem::Read(inst + off, &b, 8)) break;
        if (!mem::Read(inst + off + 8, &e, 8)) break;
        if (!mem::Read(inst + off + 16, &cap, 8)) break;
        if (b < 0x100000000ULL) continue;
        if (!(b <= e && e <= cap)) continue;
        if (b == e && e == cap) continue;              // 空 vector 没信息量
        if (cap - b > 0x1000000) continue;             // 明显不是
        const size_t nn = (size_t)(e - b);
        P("     +0x%03X  begin=0x%p end=0x%p cap=0x%p  (%zu 字节 / 每元素? )",
          off, (void*)b, (void*)e, (void*)cap, nn);
        if (++vecs >= 12) {
            P("     ...（还有更多）");
            break;
        }
    }
    if (vecs == 0) P("     （没有明显的 vector）");

    // ④ 原始浮点视图，人工兜底
    P("  -- 前 0x180 字节的浮点视图 --");
    for (int row = 0; row < 0x180; row += 16) {
        float f[4] = {0, 0, 0, 0};
        if (!mem::Read(inst + row, f, sizeof(f))) break;
        P("     +0x%03X  %12.4f %12.4f %12.4f %12.4f", row,
          f[0], f[1], f[2], f[3]);
    }
}

void ProbeClientInstanceFields(ClassResult* results, int count) {
    // 挑一个"最像真对象"的实例：地址落在堆区（栈/低地址一律排除），
    // 且 +8 是个有效指针。上一次实测里 instances[0] 是 probe 自己的
    // needles 数组（栈上），把它当 ClientInstance 分析出来全是垃圾。
    auto pick_real = [](const ClassResult* r) -> uintptr_t {
        for (int k = 0; k < r->instance_count; ++k) {
            const uintptr_t a = r->instances[k];
            if (a < 0x100000000ULL) continue;   // 栈 / 静态区
            uintptr_t f8 = 0;
            if (!mem::Read(a + 8, &f8, sizeof(f8))) continue;
            if (f8 < 0x10000) continue;
            return a;
        }
        return r->instance_count > 0 ? r->instances[0] : 0;
    };

    // 顺序有意义：ClientInstance 和 LocalPlayer 的布局已经吃透了，
    // Level / Dimension 是拿实体列表的入口，放在后面。
    const char* kOrder[] = {"ClientInstance", "LocalPlayer", "Level", "Dimension"};
    for (int oi = 0; oi < 4; ++oi) {
        const ClassResult* r = nullptr;
        for (int i = 0; i < count; ++i) {
            if (results[i].chain_ok && results[i].instance_count > 0 &&
                strcmp(results[i].name, kOrder[oi]) == 0) {
                r = &results[i];
                break;
            }
        }
        if (r == nullptr) {
            P("\n没有 %s 实例，跳过", kOrder[oi]);
            continue;
        }
        const uintptr_t a = pick_real(r);
        if (a != 0) AnalyzeObject(kOrder[oi], a, r->vtable, count, results);
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

    // ── 阶段 1：RTTI 链条 ──
    // 这一步只读主模块（快，几百毫秒），不依赖游戏状态，先把坐标定下来。
    P("\n=== RTTI 链条 ===");
    for (int i = 0; i < kClassCount; ++i) {
        g_results[i].name = kClasses[i];
        ResolveClass(&g_results[i]);
    }

    int lp_idx = -1;
    for (int i = 0; i < kClassCount; ++i) {
        if (strcmp(g_results[i].name, "LocalPlayer") == 0) lp_idx = i;
    }

    // ── 阶段 2：等 LocalPlayer 出现 ──
    // 注入时很可能还在标题界面 / 加载地形，这时候 LocalPlayer 根本不存在，
    // 全量扫一遍只会扫到一堆假阳性。所以先用单个 needle 轮询，等它出现
    // 再做完整枚举 —— 玩家一进世界，最多一个轮询周期就能抓到。
    if (lp_idx >= 0 && g_results[lp_idx].chain_ok) {
        P("\n等 LocalPlayer 出现（游戏没进世界时它不存在，每 ~3 秒探一次）...");
        const int kMaxWait = 10;   // 10 次 × 约 3~6 秒 ≈ 半分钟，够用户切进世界了
        for (int attempt = 0; attempt < kMaxWait; ++attempt) {
            const int n = FindOne(&g_results[lp_idx], g_results[lp_idx].instances, 8);
            if (n > 0) {
                g_results[lp_idx].instance_count = n;
                P("  第 %d 次探测命中 %d 个 LocalPlayer 实例", attempt + 1, n);
                break;
            }
            if (attempt % 5 == 0) {
                P("  第 %d 次探测：还没有 LocalPlayer（未进世界？）", attempt + 1);
            }
            Sleep(3000);
        }
        if (g_results[lp_idx].instance_count == 0) {
            P("  !! 等了 %d 次仍然没有 LocalPlayer，直接做全量扫描", kMaxWait);
        }
    }

    // ── 阶段 3：全量枚举 + 布局分析 ──
    FindInstances(g_results, kClassCount);
    ProbeClientInstanceFields(g_results, kClassCount);

    // ── 阶段 4：dump 主模块 ──
    // 进程内 dump 不经过 ReadProcessMemory，反作弊看不到。
    // 外部已经 dump 过一份，这里的价值是"两边对比能确认读到的内容一致"。
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
