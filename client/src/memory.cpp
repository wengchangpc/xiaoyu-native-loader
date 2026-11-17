// memory.cpp - 进程内安全读写 + 特征码扫描 + PE dump
//
// 三条设计约束，都是从"壳会把磁盘 PE 改得面目全非"这个事实推出来的：
//
//   1) 区域布局只信 VirtualQuery，不信磁盘节表。
//      磁盘上前 7 个节没有名字，`.winlice` 有 15MB 虚拟空间但 raw size 是 0。
//      按那种节表算出来的 .text 范围是错的。VirtualQuery 给的是壳解密之后
//      的真实映射，扫它才不会漏。
//
//   2) 所有读都先 VirtualQuery 预检再 memcpy，外加 SEH 兜底。
//      内存模块最忌讳"读到野指针直接崩"——那是把整个游戏带走，
//      用户看到的就是"开了某个功能游戏闪退"，根本没法定位。
//
//   3) dump 出来的 PE 让 FileAlignment == SectionAlignment。
//      于是 PointerToRawData == VirtualAddress，文件偏移就是 RVA，
//      IDA 里显示的地址和日志里打印的地址是同一个数，省掉所有换算。
#include "memory.h"

#include <windows.h>
#include <cstring>
#include <cwchar>

#include "log.h"

namespace xiaoyu {
namespace mem {
namespace {

// ── 运行时布局 ──────────────────────────────────────────────────
const int kMaxRegions = 512;
Region g_regions[kMaxRegions];
size_t g_region_count = 0;

uintptr_t g_base = 0;
size_t g_image_size = 0;
bool g_ready = false;
bool g_inited = false;

// 扫描缓存。id 是短字符串字面量，只比较指针就够（同一处调用传的是同一个
// 字面量），但为了稳妥还是比一下内容。
const int kMaxCache = 64;
struct CacheEntry {
    const char* id;
    uintptr_t value;
    bool used;
    bool resolved;   // false = 扫过但没命中（下次 RescanAll 时重试）
};
CacheEntry g_cache[kMaxCache];
int g_cache_n = 0;

// ── 可访问性检查 ────────────────────────────────────────────────
// 每个线程记一个上次命中的 region，连续读同一个对象时省掉重复 VirtualQuery。
// 指针链解引用几乎总是落在同一片堆上，命中率很高。
struct QueryCache {
    uintptr_t base = 0;
    size_t size = 0;
    bool readable = false;
    bool writable = false;
};
thread_local QueryCache t_qc;

bool ProtectReadable(DWORD p) {
    if (p & PAGE_GUARD) return false;
    switch (p & 0xFF) {
        case PAGE_NOACCESS:
            return false;
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

bool ProtectWritable(DWORD p) {
    if (p & PAGE_GUARD) return false;
    switch (p & 0xFF) {
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

// 查 [addr, addr+n) 是否整段可访问。跨 region 会逐段查。
bool Accessible(uintptr_t addr, size_t n, bool need_write) {
    if (n == 0) return true;
    if (addr < 0x10000) return false;   // 空指针区 + 前 64KB 一律不碰

    // 快速路径：完全落在上次查过的区间里
    if (t_qc.size != 0 && addr >= t_qc.base && addr + n <= t_qc.base + t_qc.size) {
        return need_write ? t_qc.writable : t_qc.readable;
    }

    uintptr_t cur = addr;
    const uintptr_t end = addr + n;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)cur, &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (!ProtectReadable(mbi.Protect)) return false;
        if (need_write && !ProtectWritable(mbi.Protect)) return false;

        const uintptr_t region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (region_end <= cur) return false;   // 防死循环
        // 记住这一段的属性，供下一次快速路径用
        t_qc.base = (uintptr_t)mbi.BaseAddress;
        t_qc.size = mbi.RegionSize;
        t_qc.readable = ProtectReadable(mbi.Protect);
        t_qc.writable = ProtectWritable(mbi.Protect);
        cur = region_end;
    }
    return true;
}

// SEH 兜底：VirtualQuery 挡不住"查完就被别的线程释放"的竞态。
// 单独一个函数，里面不出现需要析构的对象（MSVC 在 /EHsc 下不允许
// __try 出现在需要对象展开的函数里，会报 C2712）。
bool RawCopyIn(uintptr_t addr, void* out, size_t n) {
    __try {
        memcpy(out, (const void*)addr, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool RawCopyOut(uintptr_t addr, const void* src, size_t n) {
    __try {
        memcpy((void*)addr, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── 特征码 ──────────────────────────────────────────────────────
const int kMaxPat = 256;

struct Pattern {
    unsigned char bytes[kMaxPat];
    unsigned char mask[kMaxPat];   // 1 = 必须相等，0 = 通配
    int len;
    int anchor;                    // 第一个必须匹配的下标（用 memchr 加速）
    bool ok;
};

bool HexVal(char c, int* out) {
    if (c >= '0' && c <= '9') { *out = c - '0'; return true; }
    if (c >= 'a' && c <= 'f') { *out = c - 'a' + 10; return true; }
    if (c >= 'A' && c <= 'F') { *out = c - 'A' + 10; return true; }
    return false;
}

// 逐 token 解析，不先"去空格再两两切" —— 那样 '48 8B ? ? ? ?' 这种写法
// 会被压成 '488B????' 只剩 2 个通配，语义就错了。
Pattern ParsePattern(const char* s) {
    Pattern p;
    memset(&p, 0, sizeof(p));
    p.anchor = -1;
    if (s == nullptr) return p;

    int n = 0;
    const char* c = s;
    while (*c != 0) {
        while (*c == ' ' || *c == '\t' || *c == ',') ++c;
        if (*c == 0) break;

        // 取一个 token
        const char* t = c;
        while (*c != 0 && *c != ' ' && *c != '\t' && *c != ',') ++c;
        const int tlen = (int)(c - t);

        if (tlen == 1 && t[0] == '?') {
            if (n >= kMaxPat) return p;
            p.bytes[n] = 0;
            p.mask[n] = 0;
            ++n;
            continue;
        }
        if (tlen == 2 && t[0] == '?' && t[1] == '?') {
            if (n >= kMaxPat) return p;
            p.bytes[n] = 0;
            p.mask[n] = 0;
            ++n;
            continue;
        }
        if (tlen >= 2 && (tlen % 2) == 0) {
            for (int i = 0; i < tlen; i += 2) {
                int hi = 0, lo = 0;
                if (!HexVal(t[i], &hi) || !HexVal(t[i + 1], &lo)) return p;
                if (n >= kMaxPat) return p;
                p.bytes[n] = (unsigned char)((hi << 4) | lo);
                p.mask[n] = 1;
                ++n;
            }
            continue;
        }
        return p;   // 认不出来的写法，整条作废
    }

    if (n < 2) return p;
    p.len = n;
    for (int i = 0; i < n; ++i) {
        if (p.mask[i]) { p.anchor = i; break; }
    }
    if (p.anchor < 0) return p;   // 全通配没有意义
    p.ok = true;
    return p;
}

bool MatchAt(const unsigned char* data, const Pattern& p) {
    // 先比锚点字节，绝大多数位置在这里就被否掉
    if (data[p.anchor] != p.bytes[p.anchor]) return false;
    for (int i = 0; i < p.len; ++i) {
        if (!p.mask[i]) continue;
        if (data[i] != p.bytes[i]) return false;
    }
    return true;
}

// 在 [start, end) 里找第一个命中。返回绝对地址。
uintptr_t ScanOne(uintptr_t start, uintptr_t end, const Pattern& p) {
    if (!p.ok) return 0;
    const size_t plen = (size_t)p.len;
    uintptr_t cur = start;
    while (cur + plen <= end) {
        // 按 64KB 分块，块内连续比对。分块是为了让 Accessible 检查
        // 的开销摊薄（一次查一块，而不是每字节查一次）。
        const size_t chunk = 0x10000;
        size_t avail = (size_t)(end - cur);
        if (avail > chunk) avail = chunk;
        if (avail < plen) break;

        if (!Accessible(cur, avail, false)) {
            cur += avail;
            continue;
        }

        const unsigned char* data = (const unsigned char*)cur;
        size_t remaining = avail;
        const unsigned char* q = data;
        while (remaining >= plen) {
            const unsigned char* hit = (const unsigned char*)memchr(
                q + p.anchor, p.bytes[p.anchor], remaining - plen + 1);
            if (hit == nullptr) break;
            const unsigned char* cand = hit - p.anchor;
            if (MatchAt(cand, p)) {
                return (uintptr_t)cand;
            }
            q = (const unsigned char*)(hit + 1);
            remaining = (size_t)((data + avail) - q);
        }
        // 下一块从"上一块尾部往回退 plen-1"处开始，避免特征码跨块断裂
        cur += avail - (plen - 1);
        if (avail <= plen) break;
    }
    return 0;
}

}  // namespace

// ── 初始化 ──────────────────────────────────────────────────────
bool Init() {
    if (g_inited) return g_ready;
    g_inited = true;

    HMODULE mod = GetModuleHandleW(nullptr);
    if (mod == nullptr) {
        LogLine("[mem] GetModuleHandleW(nullptr) failed");
        return false;
    }
    g_base = (uintptr_t)mod;

    // Peek 一下 PE 头拿 SizeOfImage。这里不能用 Read()（还没初始化），
    // 直接 memcpy —— DOS/NT 头一定在映像头部且一定可读。
    const auto* dos = (const IMAGE_DOS_HEADER*)g_base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        LogLine("[mem] bad DOS signature 0x%04X", (unsigned)dos->e_magic);
        return false;
    }
    const auto* nt = (const IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        LogLine("[mem] bad NT signature");
        return false;
    }
    g_image_size = nt->OptionalHeader.SizeOfImage;

    // 枚举映像范围内的运行时区域。
    // 注意：外壳常把某些页设成 PAGE_NOACCESS 占位（.winlice 那 15MB），
    // 那些页扫不了也不用扫 —— 真正的代码在别的页里。
    uintptr_t cur = g_base;
    const uintptr_t end = g_base + g_image_size;
    size_t skipped_noaccess = 0;
    size_t total_committed = 0;

    while (cur < end && g_region_count < kMaxRegions) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)cur, &mbi, sizeof(mbi)) == 0) break;
        const uintptr_t rbase = (uintptr_t)mbi.BaseAddress;
        const size_t rsize = mbi.RegionSize;
        if (rsize == 0) break;
        const uintptr_t rend = rbase + rsize;

        if (mbi.State == MEM_COMMIT && ProtectReadable(mbi.Protect)) {
            Region r;
            r.base = rbase;
            r.size = rsize;
            r.protect = mbi.Protect;
            r.exec = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                     PAGE_EXECUTE_READWRITE |
                                     PAGE_EXECUTE_WRITECOPY)) != 0;
            r.image = true;
            g_regions[g_region_count++] = r;
            total_committed += rsize;
        } else if (mbi.State == MEM_COMMIT) {
            skipped_noaccess += rsize;
        }

        if (rend <= cur) break;
        cur = rend;
    }

    g_ready = g_region_count > 0;
    LogLine("[mem] base=%p image=%.1fMB regions=%d committed=%.1fMB noaccess=%.1fMB",
            (void*)g_base, (double)g_image_size / 1048576.0, (int)g_region_count,
            (double)total_committed / 1048576.0,
            (double)skipped_noaccess / 1048576.0);

    for (size_t i = 0; i < g_region_count && i < 12; ++i) {
        LogLine("[mem]  r%d %p +%.1fKB prot=0x%03X %s", (int)i,
                (void*)g_regions[i].base, (double)g_regions[i].size / 1024.0,
                (unsigned)g_regions[i].protect, g_regions[i].exec ? "exec" : "data");
    }
    return g_ready;
}

bool Ready() { return g_ready; }
uintptr_t Base() { return g_base; }
size_t ImageSize() { return g_image_size; }
size_t RegionCount() { return g_region_count; }

bool GetRegion(size_t index, Region* out) {
    if (out == nullptr || index >= g_region_count) return false;
    *out = g_regions[index];
    return true;
}

// ── 读写 ────────────────────────────────────────────────────────
bool Read(uintptr_t addr, void* out, size_t n) {
    if (out == nullptr || n == 0) return false;
    if (!Accessible(addr, n, false)) return false;
    return RawCopyIn(addr, out, n);
}

bool Write(uintptr_t addr, const void* src, size_t n) {
    if (src == nullptr || n == 0) return false;
    if (!Accessible(addr, n, true)) return false;
    if (RawCopyOut(addr, src, n)) return true;

    // 代码页大概率是只读的，需要临时改保护才能写
    DWORD old = 0;
    if (VirtualProtect((LPVOID)addr, n, PAGE_EXECUTE_READWRITE, &old) == 0) {
        return false;
    }
    const bool ok = RawCopyOut(addr, src, n);
    DWORD back = 0;
    VirtualProtect((LPVOID)addr, n, old, &back);
    return ok;
}

uintptr_t ReadPtr(uintptr_t addr) {
    uintptr_t v = 0;
    if (!Read(addr, &v, sizeof(v))) return 0;
    return v;
}

uintptr_t Follow(uintptr_t base, const int* offsets, int count) {
    uintptr_t p = base;
    for (int i = 0; i < count; ++i) {
        if (p == 0) return 0;
        p = ReadPtr(p + (intptr_t)offsets[i]);
    }
    return p;
}

bool InImage(uintptr_t addr) {
    return addr >= g_base && addr < g_base + g_image_size;
}

// ── 扫描 ────────────────────────────────────────────────────────
uintptr_t ScanRange(uintptr_t start, size_t len, const char* pattern, int skip) {
    if (!g_ready || len == 0) return 0;
    const Pattern p = ParsePattern(pattern);
    if (!p.ok) {
        LogLine("[mem] bad pattern: %s", pattern ? pattern : "(null)");
        return 0;
    }

    const uintptr_t end = start + len;
    uintptr_t cur = start;
    int left = skip < 0 ? 0 : skip;
    while (cur < end) {
        const uintptr_t hit = ScanOne(cur, end, p);
        if (hit == 0) return 0;
        if (left == 0) return hit;
        --left;
        cur = hit + 1;
    }
    return 0;
}

uintptr_t Scan(const char* pattern, int skip) {
    if (!g_ready) return 0;
    // 先扫可执行区（代码特征码绝大多数在这里），再扫数据区（找 vtable /
    // 字符串引用时会用到）。两遍分开是因为混着扫会把大量数据页也过一遍，
    // 白白多花一倍时间。
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < g_region_count; ++i) {
            const Region& r = g_regions[i];
            if (pass == 0 && !r.exec) continue;
            if (pass == 1 && r.exec) continue;
            const uintptr_t hit = ScanRange(r.base, r.size, pattern, skip);
            if (hit != 0) return hit;
        }
        // pass 0 没找到就把 skip 归零再扫数据区 —— 否则"跳过前 2 个命中"
        // 会在数据区里继续往下跳，把本来该命中的第 1 个也跳掉。
        skip = 0;
    }
    return 0;
}

uintptr_t ScanCached(const char* id, const char* pattern, int skip) {
    if (id != nullptr) {
        for (int i = 0; i < g_cache_n; ++i) {
            if (g_cache[i].id == id) {
                if (g_cache[i].resolved) return g_cache[i].value;
                // 上次扫过但没命中：允许重试一次（可能是世界还没加载完，
                // 目标对象还没实例化）。只要这次命中就永久缓存。
                const uintptr_t v = Scan(pattern, skip);
                if (v != 0) {
                    g_cache[i].value = v;
                    g_cache[i].resolved = true;
                }
                return v;
            }
        }
    }

    const uintptr_t v = Scan(pattern, skip);
    if (id != nullptr && g_cache_n < kMaxCache) {
        // 同一个 id 重复注册会浪费槽位，先查一遍
        bool dup = false;
        for (int i = 0; i < g_cache_n; ++i) {
            if (g_cache[i].id == id) { dup = true; break; }
        }
        if (!dup) {
            g_cache[g_cache_n].id = id;
            g_cache[g_cache_n].value = v;
            g_cache[g_cache_n].used = true;
            g_cache[g_cache_n].resolved = (v != 0);
            ++g_cache_n;
        }
    }
    return v;
}

void ClearScanCache() {
    for (int i = 0; i < g_cache_n; ++i) {
        g_cache[i].resolved = false;
        g_cache[i].value = 0;
    }
    t_qc = QueryCache();
    LogLine("[mem] scan cache cleared");
}

// ── 按值扫描 ────────────────────────────────────────────────────
namespace {

// 通用的"找 1/2/4/8 字节值"实现。value 已按小端读进内存比较，
// 用 memchr 锚定第一个非零字节 —— 全零值（比如 0）没有锚点，只能退化成
// 逐位置比对，那种情况很少见（没人会搜全零）。
template <int N>
uintptr_t ScanValueImpl(const unsigned char* needle, uintptr_t from, int skip) {
    int left = skip < 0 ? 0 : skip;

    for (size_t ri = 0; ri < g_region_count; ++ri) {
        const Region& r = g_regions[ri];
        if (from != 0 && r.base + r.size <= from) continue;

        uintptr_t start = r.base;
        if (from != 0 && from > start) start = from;
        const uintptr_t end = r.base + r.size;
        if (start >= end) continue;

        // 锚点：第一个非零字节
        int anchor = 0;
        while (anchor < N && needle[anchor] == 0) ++anchor;
        if (anchor >= N) anchor = 0;

        uintptr_t cur = start;
        while (cur + N <= end) {
            const size_t chunk = 0x10000;
            size_t avail = (size_t)(end - cur);
            if (avail > chunk) avail = chunk;
            if (avail < (size_t)N) break;
            if (!Accessible(cur, avail, false)) {
                cur += avail;
                continue;
            }
            const unsigned char* data = (const unsigned char*)cur;
            size_t remaining = avail;
            const unsigned char* q = data;
            while (remaining >= (size_t)N) {
                const unsigned char* hit = (const unsigned char*)memchr(
                    q + anchor, needle[anchor], remaining - N + 1);
                if (hit == nullptr) break;
                const unsigned char* cand = hit - anchor;
                if (memcmp(cand, needle, N) == 0) {
                    if (left == 0) {
                        return (uintptr_t)cand;
                    }
                    --left;
                }
                q = (const unsigned char*)(hit + 1);
                remaining = (size_t)((data + avail) - q);
            }
            cur += avail - (N - 1);
            if (avail <= (size_t)N) break;
        }
    }
    return 0;
}

}  // namespace

uintptr_t ScanDword(uint32_t value, uintptr_t from, int skip) {
    if (!g_ready) return 0;
    unsigned char n[4];
    memcpy(n, &value, 4);
    return ScanValueImpl<4>(n, from, skip);
}

uintptr_t ScanQword(uint64_t value, uintptr_t from, int skip) {
    if (!g_ready) return 0;
    unsigned char n[8];
    memcpy(n, &value, 8);
    return ScanValueImpl<8>(n, from, skip);
}

int FindAllQword(uint64_t value, uintptr_t* out, int max,
                 uintptr_t from, uintptr_t to) {
    if (!g_ready || out == nullptr || max <= 0) return 0;
    int n = 0;
    uintptr_t cur = from;
    while (n < max) {
        const uintptr_t hit = ScanQword(value, cur, 0);
        if (hit == 0) break;
        if (to != 0 && hit >= to) break;
        out[n++] = hit;
        cur = hit + 1;
    }
    return n;
}

uintptr_t ScanBytes(const void* bytes, size_t n, int skip) {
    if (!g_ready || bytes == nullptr || n == 0 || n > 4096) return 0;
    const unsigned char* needle = (const unsigned char*)bytes;

    int anchor = 0;
    while (anchor < (int)n && needle[anchor] == 0) ++anchor;
    if (anchor >= (int)n) anchor = 0;

    int left = skip < 0 ? 0 : skip;

    for (size_t ri = 0; ri < g_region_count; ++ri) {
        const Region& r = g_regions[ri];
        uintptr_t cur = r.base;
        const uintptr_t end = r.base + r.size;
        while (cur + n <= end) {
            const size_t chunk = 0x10000;
            size_t avail = (size_t)(end - cur);
            if (avail > chunk) avail = chunk;
            if (avail < n) break;
            if (!Accessible(cur, avail, false)) {
                cur += avail;
                continue;
            }
            const unsigned char* data = (const unsigned char*)cur;
            size_t remaining = avail;
            const unsigned char* q = data;
            while (remaining >= n) {
                const unsigned char* hit = (const unsigned char*)memchr(
                    q + anchor, needle[anchor], remaining - n + 1);
                if (hit == nullptr) break;
                const unsigned char* cand = hit - anchor;
                if (memcmp(cand, needle, n) == 0) {
                    if (left == 0) return (uintptr_t)cand;
                    --left;
                }
                q = (const unsigned char*)(hit + 1);
                remaining = (size_t)((data + avail) - q);
            }
            cur += avail - (n - 1);
            if (avail <= n) break;
        }
    }
    return 0;
}

uintptr_t ScanText(const char* ascii, int skip) {
    if (ascii == nullptr) return 0;
    return ScanBytes(ascii, strlen(ascii), skip);
}

// ── 堆扫描 ──────────────────────────────────────────────────────
namespace {

// 遍历整个进程的私有可写区域。fn 拿到的每一块保证可读。
//
// ★ 必须跳过当前线程的栈：扫堆找对象的调用方会在栈上放一份"要找的 vtable
//   数组"（比如 needles[]），而栈同样是 MEM_PRIVATE + 可写。不跳过的话，
//   那个数组会被当成"一堆对象首字段连续排列"，命中结果排在真对象前面，
//   白白污染输出。实测就是这个坑：probe 把自己的 needles 数组报成了
//   ClientInstance 实例。
template <class Fn>
void ForEachPrivateRegion(Fn fn, size_t* scanned) {
    const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();
    const uintptr_t stack_lo = (uintptr_t)tib->StackLimit;
    const uintptr_t stack_hi = (uintptr_t)tib->StackBase;

    uintptr_t cur = 0x10000;
    const uintptr_t limit = (uintptr_t)1 << 47;
    size_t total = 0;

    while (cur < limit) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)cur, &mbi, sizeof(mbi)) == 0) break;
        const uintptr_t base = (uintptr_t)mbi.BaseAddress;
        const size_t size = mbi.RegionSize;
        if (size == 0) break;

        const uintptr_t rend = base + size;
        const bool is_own_stack = (rend > stack_lo) && (base < stack_hi);

        if (!is_own_stack && mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            ProtectReadable(mbi.Protect) && ProtectWritable(mbi.Protect)) {
            total += size;
            fn(base, size);
        }

        if (rend <= cur) break;
        cur = rend;
    }
    if (scanned != nullptr) *scanned = total;
}

}  // namespace

int ScanHeapQword(const uint64_t* needles, int needle_count,
                  HeapHit* out, int max_out, size_t* scanned_bytes) {
    if (needles == nullptr || needle_count <= 0 || out == nullptr || max_out <= 0) {
        return 0;
    }

    // 每个 needle 先转成字节串。堆里逐块读出来之后用 memchr 锚定首个
    // 非零字节再比对 —— 和 ScanValueImpl 同样的加速思路。
    struct Needle {
        unsigned char bytes[8];
        int anchor;
        int index;
    };
    Needle ns[16];
    const int nc = needle_count > 16 ? 16 : needle_count;
    for (int i = 0; i < nc; ++i) {
        memcpy(ns[i].bytes, &needles[i], 8);
        int a = 0;
        while (a < 8 && ns[i].bytes[a] == 0) ++a;
        ns[i].anchor = (a >= 8) ? 0 : a;
        ns[i].index = i;
    }

    int found = 0;
    const size_t kChunk = 4 << 20;

    ForEachPrivateRegion(
        [&](uintptr_t base, size_t size) {
            if (found >= max_out) return;
            uintptr_t off = 0;
            while (off < size && found < max_out) {
                size_t n = kChunk;
                if (n > size - off) n = size - off;
                const uintptr_t addr = base + off;
                if (Accessible(addr, n, false)) {
                    const unsigned char* data = (const unsigned char*)addr;
                    for (int i = 0; i < nc && found < max_out; ++i) {
                        const size_t remaining = n;
                        if (remaining < 8) break;
                        const unsigned char* q = data;
                        size_t left = remaining;
                        while (left >= 8 && found < max_out) {
                            const unsigned char* hit = (const unsigned char*)memchr(
                                q + ns[i].anchor, ns[i].bytes[ns[i].anchor],
                                left - 8 + 1);
                            if (hit == nullptr) break;
                            const unsigned char* cand = hit - ns[i].anchor;
                            if (memcmp(cand, ns[i].bytes, 8) == 0) {
                                out[found].addr = (uintptr_t)cand;
                                out[found].index = ns[i].index;
                                ++found;
                            }
                            q = (const unsigned char*)(hit + 1);
                            left = (size_t)((data + remaining) - q);
                        }
                    }
                }
                off += n;
            }
        },
        scanned_bytes);

    return found;
}

int ScanHeapPattern(const char* pattern, uintptr_t* out, int max_out,
                    size_t* scanned_bytes) {
    const Pattern p = ParsePattern(pattern);
    if (!p.ok || out == nullptr || max_out <= 0) return 0;

    int found = 0;
    const size_t kChunk = 4 << 20;
    const size_t plen = (size_t)p.len;

    ForEachPrivateRegion(
        [&](uintptr_t base, size_t size) {
            if (found >= max_out) return;
            uintptr_t off = 0;
            while (off < size && found < max_out) {
                size_t n = kChunk;
                if (n > size - off) n = size - off;
                const uintptr_t addr = base + off;
                if (Accessible(addr, n, false) && n >= plen) {
                    const unsigned char* data = (const unsigned char*)addr;
                    size_t left = n;
                    const unsigned char* q = data;
                    while (left >= plen && found < max_out) {
                        const unsigned char* hit = (const unsigned char*)memchr(
                            q + p.anchor, p.bytes[p.anchor], left - plen + 1);
                        if (hit == nullptr) break;
                        const unsigned char* cand = hit - p.anchor;
                        if (MatchAt(cand, p)) {
                            out[found++] = (uintptr_t)cand;
                        }
                        q = (const unsigned char*)(hit + 1);
                        left = (size_t)((data + n) - q);
                    }
                }
                off += n;
            }
        },
        scanned_bytes);

    return found;
}

// ── RIP 相对寻址 ────────────────────────────────────────────────
uintptr_t ResolveRip(uintptr_t at, int instr_len, int disp_off) {
    if (at == 0 || instr_len <= 0 || disp_off < 0) return 0;
    if (disp_off + 4 > instr_len) return 0;
    int32_t disp = 0;
    if (!Read(at + disp_off, &disp, sizeof(disp))) return 0;
    return at + (uintptr_t)instr_len + (intptr_t)disp;
}

uintptr_t RipTarget(uintptr_t at, int instr_len) {
    if (instr_len < 4) return 0;
    return ResolveRip(at, instr_len, instr_len - 4);
}

// ── 校验 ────────────────────────────────────────────────────────
const int kMaxChecks = 32;
Check g_checks[kMaxChecks];
int g_check_n = 0;

bool AddCheck(const char* what, bool ok) {
    if (g_check_n < kMaxChecks) {
        g_checks[g_check_n].what = what;
        g_checks[g_check_n].ok = ok;
        ++g_check_n;
    }
    if (!ok) {
        LogLine("[mem] CHECK FAILED: %s", what ? what : "?");
    }
    return ok;
}

int FailedCheckCount() {
    int n = 0;
    for (int i = 0; i < g_check_n; ++i) {
        if (!g_checks[i].ok) ++n;
    }
    return n;
}

const char* FirstFailedCheck() {
    for (int i = 0; i < g_check_n; ++i) {
        if (!g_checks[i].ok) return g_checks[i].what;
    }
    return nullptr;
}

// ── dump ────────────────────────────────────────────────────────
const wchar_t* DumpDir() { return L"C:\\Xiaoyu\\dump\\"; }

namespace {

bool WriteWhole(const wchar_t* path, const void* data, size_t n) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LogLine("[dump] CreateFile failed err=%lu", (unsigned long)GetLastError());
        return false;
    }
    const char* p = (const char*)data;
    size_t left = n;
    while (left > 0) {
        DWORD chunk = (left > 0x4000000) ? 0x4000000 : (DWORD)left;
        DWORD wrote = 0;
        if (WriteFile(h, p, chunk, &wrote, nullptr) == 0 || wrote == 0) {
            LogLine("[dump] WriteFile failed err=%lu", (unsigned long)GetLastError());
            CloseHandle(h);
            return false;
        }
        p += wrote;
        left -= wrote;
    }
    CloseHandle(h);
    return true;
}

}  // namespace

bool DumpImage(const wchar_t* path) {
    if (!g_ready) return false;
    if (path == nullptr) path = L"C:\\Xiaoyu\\dump\\main.dll";

    const size_t total = g_image_size;
    if (total < 0x1000 || total > 0x40000000) {   // 上限 1GB，防手滑
        LogLine("[dump] image size looks wrong: %zu", total);
        return false;
    }

    auto* buf = (unsigned char*)VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE,
                                             PAGE_READWRITE);
    if (buf == nullptr) {
        LogLine("[dump] VirtualAlloc %zu failed", total);
        return false;
    }
    memset(buf, 0, total);

    // 逐区域把运行时内容铺到 RVA 位置。
    // 用区域而不是节表：壳改了保护属性的页会被 VirtualQuery 拆成多段，
    // 按区域走能一段不漏。
    size_t copied = 0;
    for (size_t i = 0; i < g_region_count; ++i) {
        const Region& r = g_regions[i];
        if (r.base < g_base) continue;
        const size_t off = (size_t)(r.base - g_base);
        if (off >= total) continue;
        size_t n = r.size;
        if (off + n > total) n = total - off;
        if (n == 0) continue;
        if (RawCopyIn(r.base, buf + off, n)) copied += n;
    }

    // ── 重写 PE 头 ──
    // 保留原来的 DOS/NT 头（里面有入口点、数据目录等 IDA 需要的字段），
    // 只动跟"文件布局"有关的部分。
    auto* dos = (IMAGE_DOS_HEADER*)buf;
    auto* nt = (IMAGE_NT_HEADERS64*)(buf + dos->e_lfanew);
    const WORD nsect = nt->FileHeader.NumberOfSections;

    const DWORD sec_align = 0x1000;
    nt->OptionalHeader.FileAlignment = sec_align;
    nt->OptionalHeader.SectionAlignment = sec_align;
    nt->OptionalHeader.SizeOfHeaders = 0x1000;

    // 每个节：raw 偏移直接等于 VA，raw 大小按节对齐取整。
    // 这样文件里的 0x1000 就是 RVA 0x1000，IDA 加载后地址完全对得上。
    auto* sec = IMAGE_FIRST_SECTION(nt);
    size_t max_raw_end = 0x1000;
    for (WORD i = 0; i < nsect; ++i) {
        const size_t va = sec[i].VirtualAddress;
        size_t vs = sec[i].Misc.VirtualSize;
        if (vs == 0) vs = sec[i].SizeOfRawData;
        size_t raw = (vs + sec_align - 1) & ~(size_t)(sec_align - 1);
        if (raw == 0) raw = sec_align;
        if (va >= total) {           // 节表本身被壳改坏了，跳过
            sec[i].PointerToRawData = 0;
            sec[i].SizeOfRawData = 0;
            continue;
        }
        if (va + raw > total) raw = total - va;
        sec[i].PointerToRawData = (DWORD)va;
        sec[i].SizeOfRawData = (DWORD)raw;
        sec[i].Misc.VirtualSize = (DWORD)vs;
        const size_t e = va + raw;
        if (e > max_raw_end) max_raw_end = e;
    }
    nt->OptionalHeader.SizeOfImage = (DWORD)((total + sec_align - 1) & ~(size_t)(sec_align - 1));

    // 节表里一个有效节都没有（壳把节表整个抹了）→ 造一个覆盖全映像的节，
    // 否则 IDA 会因为"没有可加载的节"直接拒绝打开。
    bool any = false;
    for (WORD i = 0; i < nsect; ++i) {
        if (sec[i].SizeOfRawData != 0) { any = true; break; }
    }
    if (!any) {
        memset(sec, 0, sizeof(IMAGE_SECTION_HEADER) * nsect);
        memcpy(sec[0].Name, ".dump", 5);
        sec[0].Misc.VirtualSize = (DWORD)(total - sec_align);
        sec[0].VirtualAddress = sec_align;
        sec[0].SizeOfRawData = (DWORD)((total - sec_align + sec_align - 1) &
                                       ~(size_t)(sec_align - 1));
        sec[0].PointerToRawData = sec_align;
        sec[0].Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE |
                                 IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA |
                                 IMAGE_SCN_MEM_WRITE;
        nt->FileHeader.NumberOfSections = 1;
        max_raw_end = total;
    }

    const bool ok = WriteWhole(path, buf, max_raw_end);
    VirtualFree(buf, 0, MEM_RELEASE);

    if (ok) {
        LogLine("[dump] wrote %ls (%zu bytes, copied %.1fMB, %u sections)",
                path, max_raw_end, (double)copied / 1048576.0, (unsigned)nsect);
    }
    return ok;
}

bool DumpRange(const wchar_t* path, uintptr_t start, size_t len) {
    if (path == nullptr || len == 0) return false;
    auto* buf = (unsigned char*)VirtualAlloc(nullptr, len, MEM_COMMIT | MEM_RESERVE,
                                             PAGE_READWRITE);
    if (buf == nullptr) return false;
    memset(buf, 0, len);

    size_t got = 0;
    uintptr_t cur = start;
    const uintptr_t end = start + len;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)cur, &mbi, sizeof(mbi)) == 0) break;
        const uintptr_t rbase = (uintptr_t)mbi.BaseAddress;
        const uintptr_t rend = rbase + mbi.RegionSize;
        if (rend <= cur) break;
        if (mbi.State == MEM_COMMIT && ProtectReadable(mbi.Protect)) {
            const uintptr_t from = cur > rbase ? cur : rbase;
            const uintptr_t to = rend < end ? rend : end;
            if (to > from) {
                const size_t n = (size_t)(to - from);
                if (RawCopyIn(from, buf + (from - start), n)) got += n;
            }
        }
        cur = rend < end ? rend : end;
    }

    const bool ok = WriteWhole(path, buf, len);
    VirtualFree(buf, 0, MEM_RELEASE);
    if (ok) {
        LogLine("[dump] wrote %ls (%zu bytes, %.1fKB readable)", path, len,
                (double)got / 1024.0);
    }
    return ok;
}

}  // namespace mem
}  // namespace xiaoyu
