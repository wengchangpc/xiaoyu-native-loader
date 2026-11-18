// explorer.cpp - 全堆实体枚举 + 容器反查
//
// 两遍扫描，各自只看一件事：
//   第一遍：把每个 8 字节对齐的 qword 当"对象首地址候选"，如果它的值是主模块
//           vtable 区里的地址，就把这个位置当成对象开头，验 Actor 布局。
//   第二遍：拿第一遍得到的 Actor 地址集合，反过来扫全堆，看哪些位置存着这些
//           指针。同一容器里的指针会在地址上连续排列 —— 容器自己就露出来了。
//
// 性能取舍：
//   1.5 亿个 qword 的线性扫描，顺序读 1.2GB 大约受内存带宽限制，实测秒级。
//   第一遍里只有"值落在 vtable 区"的位置才会去验坐标（约万分之几），
//   所以随机读的次数很少，不会拖慢主循环。
//   步长用 8 而不是 16：MSVC 的对象首地址确实至少 16 对齐，但一旦哪天遇到
//   8 对齐的分配就整类漏掉，代价不对称 —— 宁可多扫一倍。
#include "explorer.h"

#include <windows.h>
#include <intrin.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "log.h"
#include "memory.h"

namespace xiaoyu {
namespace explorer {
namespace {

// ── 日志 ────────────────────────────────────────────────────────
FILE* g_pf = nullptr;

void POpen() {
    if (g_pf != nullptr) return;
    CreateDirectoryW(L"C:\\Xiaoyu", nullptr);
    CreateDirectoryW(L"C:\\Xiaoyu\\dump", nullptr);
    g_pf = fopen("C:\\Xiaoyu\\dump\\explorer.log", "wb");
    if (g_pf == nullptr) g_pf = fopen("explorer.log", "wb");
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

// ── 计时 ────────────────────────────────────────────────────────
LARGE_INTEGER g_freq;

double MsSince(const LARGE_INTEGER& t0) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_freq.QuadPart == 0) return 0.0;
    return (double)(now.QuadPart - t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

// ── Actor 布局常量（全部实测，不是猜的）────────────────────────
//   LocalPlayer 上差分扫描（12 次 × 0.35s）确认的：
//     +0x368  float 0.6          碰撞箱宽度，常量锚点
//     +0xCE0  Vec3  mPos         当前位置，三连变化幅度 0.674 / 1.987 / 8.683
//     +0xCEC  Vec3  mPosPrev     上一帧位置，与 mPos 交替变化
//     +0xDE8  BlockSource*       指向该 Actor 所在维度的方块访问器
const uint32_t kPosOff = 0x0CE0;
const uint32_t kPosPrevOff = 0x0CEC;
const uint32_t kBlockSourceOff = 0x0DE8;

// 要读到 BlockSource 的最后一个字节，所以对象至少要有这么长
const uint32_t kMinTail = kBlockSourceOff + 8;

const int kMaxActors = 4096;
const int kMaxRefs = 16384;

struct ActorHit {
    uintptr_t addr;
    uintptr_t vtable;
    float pos[3];
    float prev[3];
    uintptr_t block_source;
};

ActorHit g_actors[kMaxActors];
int g_actor_count = 0;
bool g_overflow = false;

struct RefHit {
    uintptr_t at;
    uintptr_t value;
};
RefHit g_refs[kMaxRefs];
int g_ref_count = 0;
bool g_ref_overflow = false;

// ── 安全读 ──────────────────────────────────────────────────────
// 单独一个函数：MSVC 在 /EHsc 下不允许 __try 出现在需要对象展开的函数里
// （C2712），所以这里不能出现任何带析构函数的东西。
bool CopySafe(uintptr_t addr, void* out, size_t n) {
    __try {
        memcpy(out, (const void*)addr, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadPtrSafe(uintptr_t addr, uintptr_t* out) {
    return CopySafe(addr, out, sizeof(uintptr_t));
}

// ── 坐标判定 ────────────────────────────────────────────────────
bool PlausibleVec3(const float* v) {
    if (!(v[0] == v[0] && v[1] == v[1] && v[2] == v[2])) return false;   // NaN
    const float kInf = 3.0e38f;
    if (v[0] > kInf || v[0] < -kInf) return false;
    if (v[1] > kInf || v[1] < -kInf) return false;
    if (v[2] > kInf || v[2] < -kInf) return false;
    if (v[1] < -80.0f || v[1] > 400.0f) return false;                    // 世界高度
    if (fabsf(v[0]) > 3.0e7f || fabsf(v[2]) > 3.0e7f) return false;      // 世界边界
    // 原点附近的 (0,0,0) 几乎一定是未初始化的内存
    if (fabsf(v[0]) < 0.01f && fabsf(v[1]) < 0.01f && fabsf(v[2]) < 0.01f) return false;
    return true;
}

// ── 校验 +0xDE8 是不是真的 BlockSource ─────────────────────────
// 这是整套判据里最硬的一条，也是第一版翻车之后补上的。
//
// 第一版只验了"mPos / mPosPrev 两个位置都是合法坐标且距离 < 64"，
// 结果扫出 1024 个假阳性打成上限，一个真 Actor 都没有：
//   UBlockComponentDescription / VITrait::BlockTrait /
//   UOrbitalDirectionalLightRenderData ...
// 这些对象的 +0xCE0 附近恰好是 0.298 / 1.0 / 0.6 这类归一化浮点参数，
// 全部落在 [-80, 400] 里，两点距离也小于 64 —— 判据根本挡不住。
//
// BlockSource 这一条几乎不可能误报：要求 +0xDE8 是一个堆指针，而且
// 它指向的对象的 vtable 反查出来必须是 "BlockSource"。随机数据碰不上。
const char* ClassOf(uintptr_t vt);   // 前置声明，定义在下面

bool IsBlockSourcePtr(uintptr_t p) {
    if (p < 0x100000000ULL || p >= 0x7FFF00000000ULL) return false;
    uintptr_t vt = 0;
    if (!ReadPtrSafe(p, &vt)) return false;
    const char* nm = ClassOf(vt);
    return nm != nullptr && strcmp(nm, "BlockSource") == 0;
}

// ── 判定一个地址是不是 Actor ────────────────────────────────────
void TryActor(uintptr_t obj) {
    if (g_actor_count >= kMaxActors) {
        g_overflow = true;
        return;
    }

    float pos[3];
    float prev[3];
    if (!CopySafe(obj + kPosOff, pos, sizeof(pos))) return;
    if (!PlausibleVec3(pos)) return;
    if (!CopySafe(obj + kPosPrevOff, prev, sizeof(prev))) return;
    if (!PlausibleVec3(prev)) return;

    // mPos 与 mPosPrev 只差一帧。一帧跑 64 米不可能，这个条件把
    // "两个独立随机浮点恰好都合法"的情况彻底排掉。
    const float dx = pos[0] - prev[0];
    const float dy = pos[1] - prev[1];
    const float dz = pos[2] - prev[2];
    if (dx * dx + dy * dy + dz * dz > 64.0f * 64.0f) return;

    uintptr_t bs = 0;
    if (!ReadPtrSafe(obj + kBlockSourceOff, &bs)) return;
    if (!IsBlockSourcePtr(bs)) return;

    ActorHit* h = &g_actors[g_actor_count];
    h->addr = obj;
    h->vtable = 0;
    ReadPtrSafe(obj, &h->vtable);
    memcpy(h->pos, pos, sizeof(pos));
    memcpy(h->prev, prev, sizeof(prev));
    h->block_source = bs;

    ++g_actor_count;
}

// ── 类型名反查（vtable -> COL -> TypeDescriptor）───────────────
// 逻辑和 probe.cpp 里那套同源，但这里省掉了自校验：调用方拿到的 vtable
// 已经落在"主模块 vtable 区"里了，错不到哪去。解析失败返回 nullptr，
// 输出成 '?'，不影响主流程。
const char* ClassOf(uintptr_t vt) {
    const uintptr_t base = mem::Base();
    if (base == 0) return nullptr;
    if (vt < base || vt >= base + mem::ImageSize()) return nullptr;

    uintptr_t col = 0;
    if (!ReadPtrSafe(vt - 8, &col)) return nullptr;
    if (col < base || col >= base + mem::ImageSize()) return nullptr;

    uint32_t sig = 0;
    if (!CopySafe(col, &sig, 4)) return nullptr;
    if (sig != 1) return nullptr;

    uint32_t td_rva = 0;
    if (!CopySafe(col + 12, &td_rva, 4)) return nullptr;
    if (td_rva == 0) return nullptr;

    static char buf[192];
    if (!CopySafe(base + td_rva + 16, buf, sizeof(buf) - 1)) return nullptr;
    buf[sizeof(buf) - 1] = 0;

    // ".?AVClientInstance@@" -> "ClientInstance"
    const char* s = buf;
    if (s[0] == '.' && s[1] == '?' && s[2] == 'A') s += 3;
    char* w = buf;
    while (*s != 0 && s < buf + sizeof(buf) - 3) {
        if (*s == '@' && s[1] == '@') break;
        if (*s == '@') {
            *w++ = ':';
            *w++ = ':';
            ++s;
            continue;
        }
        *w++ = *s++;
    }
    *w = 0;
    return buf;
}

// ── 区域遍历 ────────────────────────────────────────────────────
struct Range {
    uintptr_t base;
    size_t size;
};

// 遍历所有 MEM_COMMIT + MEM_PRIVATE + 可写 的区域，对每一块调用 fn，
// 返回扫过的总字节数。
//
// ★ 不能用"先收集到数组再扫"的写法：区域数量会超过任何固定上限。
//   上一版用 Range g_ranges[4096] 收集，实际区域数正好撞上 4096 ——
//   列表被静默截断，只扫了 861MB 就停了，LocalPlayer 所在的那段堆根本
//   没被扫到。于是 1024 个命中名额全被前半段的假阳性占满，
//   最后一行的 LocalPlayer 是 0x0。改成回调式之后没有上限。
//
// 跳过当前线程的栈：调用方在栈上放了临时数组，里面可能有"看起来像对象"的值。
template <class Fn>
size_t ForEachWritableRegion(Fn fn) {
    // TEB 自指针在 GS:0x30，TIB 的 StackBase 在 +0x08、StackLimit 在 +0x10
    const uintptr_t teb = (uintptr_t)__readgsqword(0x30);
    uintptr_t stack_base = 0;
    uintptr_t stack_limit = 0;
    if (teb != 0) {
        (void)CopySafe(teb + 0x08, &stack_base, sizeof(stack_base));
        (void)CopySafe(teb + 0x10, &stack_limit, sizeof(stack_limit));
    }

    size_t total = 0;
    uintptr_t cur = 0x10000;
    const uintptr_t limit = (uintptr_t)1 << 47;

    while (cur < limit) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)cur, &mbi, sizeof(mbi)) == 0) break;

        const uintptr_t rbase = (uintptr_t)mbi.BaseAddress;
        const size_t size = mbi.RegionSize;
        if (size == 0) break;

        const uintptr_t rend = rbase + size;
        const bool own_stack = (rend > stack_limit) && (rbase < stack_base);

        const uint32_t pr = mbi.Protect;
        const bool writable =
            (pr == PAGE_READWRITE || pr == PAGE_WRITECOPY ||
             pr == PAGE_EXECUTE_READWRITE || pr == PAGE_EXECUTE_WRITECOPY);

        if (!own_stack && mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && writable) {
            total += size;
            fn(rbase, size);
        }

        if (rend <= cur) break;
        cur = rend;
    }
    return total;
}

// ── 第一遍：找 Actor ────────────────────────────────────────────
void PassActors(uintptr_t base, size_t size, uintptr_t vt_lo, uintptr_t vt_hi) {
    if (size <= kMinTail) return;
    const uintptr_t end = base + size;
    // 只有"从 p 往后还能读到 BlockSource 字段"的位置才值得验
    const uintptr_t last = end - kMinTail;

    uintptr_t p = (base + 7) & ~(uintptr_t)7;
    while (p < last) {
        const uint64_t v = *(const uint64_t*)p;
        if (v >= (uint64_t)vt_lo && v < (uint64_t)vt_hi) {
            TryActor(p);
        }
        p += 8;
    }
}

// ── 第二遍：找"存着 Actor 指针的位置"──────────────────────────
// 开放寻址哈希。Actor 最多 1024 个，用 8192 槽，负载因子 ≤ 0.125，
// 绝大多数查询第一次探测就命中或判定不在表里。
const int kHashBits = 13;
const int kHashSize = 1 << kHashBits;
const int kHashMask = kHashSize - 1;
uint64_t g_hash[kHashSize];

inline uint32_t HashSlot(uint64_t v) {
    v *= 0x9E3779B97F4A7C15ULL;
    return (uint32_t)((v >> (64 - kHashBits)) & (uint64_t)kHashMask);
}

void HashBuild() {
    memset(g_hash, 0, sizeof(g_hash));
    for (int i = 0; i < g_actor_count; ++i) {
        const uint64_t v = (uint64_t)g_actors[i].addr;
        uint32_t k = HashSlot(v);
        for (int probe = 0; probe < kHashSize; ++probe) {
            if (g_hash[k] == 0) {
                g_hash[k] = v;
                break;
            }
            if (g_hash[k] == v) break;
            k = (k + 1) & (uint32_t)kHashMask;
        }
    }
}

inline bool HashHas(uint64_t v) {
    if (v == 0) return false;
    uint32_t k = HashSlot(v);
    for (int probe = 0; probe < 32; ++probe) {
        const uint64_t slot = g_hash[k];
        if (slot == 0) return false;
        if (slot == v) return true;
        k = (k + 1) & (uint32_t)kHashMask;
    }
    return false;
}

void PassRefs(uintptr_t base, size_t size) {
    if (size < 8) return;
    const uintptr_t end = base + size;
    uintptr_t p = (base + 7) & ~(uintptr_t)7;
    while (p + 8 <= end) {
        const uint64_t v = *(const uint64_t*)p;
        if (HashHas(v)) {
            if (g_ref_count < kMaxRefs) {
                g_refs[g_ref_count].at = p;
                g_refs[g_ref_count].value = (uintptr_t)v;
                ++g_ref_count;
            } else {
                g_ref_overflow = true;
            }
        }
        p += 8;
    }
}

// 引用按地址升序排 —— 只有这样"相邻 8 字节"才等于"同一容器里连续排列"
void SortRefs() {
    for (int i = 1; i < g_ref_count; ++i) {
        const RefHit key = g_refs[i];
        int j = i - 1;
        while (j >= 0 && g_refs[j].at > key.at) {
            g_refs[j + 1] = g_refs[j];
            --j;
        }
        g_refs[j + 1] = key;
    }
}

// ── 线程入口 ────────────────────────────────────────────────────
DWORD WINAPI ThreadProc(LPVOID param) {
    const unsigned delay = (unsigned)(uintptr_t)param;
    Sleep(delay);
    Run();
    return 0;
}

}  // namespace

int ActorCount() { return g_actor_count; }

void Run() {
    POpen();
    QueryPerformanceFrequency(&g_freq);

    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);

    P("");
    P("############################################################");
    P("# explorer: 全堆实体枚举");
    P("############################################################");

    if (!mem::Init()) {
        P("mem::Init 失败，退出");
        PClose();
        return;
    }

    const uintptr_t base = mem::Base();
    const size_t image = mem::ImageSize();
    P("base        = 0x%llX", (unsigned long long)base);
    P("ImageSize   = 0x%llX", (unsigned long long)image);

    // vtable 区范围。实测所有已确认的 vtable RVA 落在 0x8A00000 ~ 0x9200000：
    //   ClientInstance 0x8B9B6F0 / LocalPlayer 0x8C98B08 / Actor 0x8F3AA30
    //   Level 0x8FE38C0 / Dimension 0x908EE48 / Container 0x8ACE800
    //
    // ★ 不再做"0 命中就放宽范围重试"。上一版就是这么崩的：第一遍扫 861MB
    //   没命中，紧接着放宽范围再扫一遍，第二遍扫到一半整个游戏消失。
    //   连续两次全堆遍历会被反作弊盯上 —— 一次都没命中的时候宁可老实报 0，
    //   也不要偷偷再扫一遍把进程搭进去。范围本身留足余量（0x8600000 ~
    //   0x9600000，16MB），一次覆盖全部实测 vtable。
    uintptr_t vt_lo = base + 0x08600000ULL;
    uintptr_t vt_hi = base + 0x09600000ULL;
    if (vt_hi > base + image) vt_hi = base + image;
    P("vtable 区   = 0x%llX ~ 0x%llX", (unsigned long long)vt_lo, (unsigned long long)vt_hi);

    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);

    // ── 第一遍：找 Actor（边遍历边扫，不预先收集区域列表）──
    // 每 128MB 打一行进度：扫堆过程中进程随时可能被反作弊杀掉，
    // 没有进度日志就只能对着"什么都没写"猜崩在哪一步。
    g_actor_count = 0;
    g_overflow = false;
    size_t done_bytes = 0;
    size_t next_mark = 128ull << 20;
    const size_t total = ForEachWritableRegion([&](uintptr_t rbase, size_t rsize) {
        PassActors(rbase, rsize, vt_lo, vt_hi);
        done_bytes += rsize;
        if (done_bytes >= next_mark) {
            P("  ...已扫 %.0f MB, 命中 %d", (double)done_bytes / 1048576.0, g_actor_count);
            next_mark += (128ull << 20);
        }
    });
    P("可写私有区  = %.1f MB（边遍历边扫，无区域数量上限）", (double)total / 1048576.0);

    P("");
    P("第一遍：%d 个 Actor%s  (%.0f ms)",
      g_actor_count, g_overflow ? "（已截断）" : "", MsSince(t1));

    // ── 输出 Actor 列表 ──
    P("");
    P("=== ACTOR LIST ===");
    const int kPrintMax = 200;   // 上千行日志既刷屏又拖慢写盘，只打前 200
    // ★ LocalPlayer 必须在**全量**里找。上一版把它放在只打前 200 行的循环里，
    //   于是命中一旦超过 200 就再也检测不到它。
    uintptr_t local_player = 0;
    for (int i = 0; i < g_actor_count; ++i) {
        const char* nm = ClassOf(g_actors[i].vtable);
        if (nm != nullptr && strcmp(nm, "LocalPlayer") == 0) {
            local_player = g_actors[i].addr;
            break;
        }
    }
    for (int i = 0; i < g_actor_count && i < kPrintMax; ++i) {
        const ActorHit& h = g_actors[i];
        const char* nm = ClassOf(h.vtable);
        P("addr=0x%llX vt=0x%llX class=%s pos=%.3f,%.3f,%.3f prev=%.3f,%.3f,%.3f bs=0x%llX",
          (unsigned long long)h.addr, (unsigned long long)h.vtable,
          nm != nullptr ? nm : "?",
          h.pos[0], h.pos[1], h.pos[2],
          h.prev[0], h.prev[1], h.prev[2],
          (unsigned long long)h.block_source);
    }
    P("=== END ACTOR LIST ===");
    P("");
    P("LocalPlayer = 0x%llX", (unsigned long long)local_player);

    // BlockSource 一致性：同一个维度的所有 Actor 应该共用同一个 BlockSource。
    // 这条能反过来验证 +0xDE8 对所有 Actor 都成立，而不只是 LocalPlayer。
    P("");
    P("=== BlockSource 一致性 ===");
    const int kConsistMax = 256;   // O(n^2)，n 大了会卡
    const int consist_n = g_actor_count < kConsistMax ? g_actor_count : kConsistMax;
    for (int i = 0; i < consist_n; ++i) {
        const uintptr_t bs = g_actors[i].block_source;
        if (bs == 0) continue;
        int same = 0;
        for (int j = 0; j < consist_n; ++j) {
            if (g_actors[j].block_source == bs) ++same;
        }
        // 只报第一出现的那次，避免同一个值打印 20 遍
        bool first = true;
        for (int j = 0; j < i; ++j) {
            if (g_actors[j].block_source == bs) {
                first = false;
                break;
            }
        }
        if (first && same > 1) {
            P("  bs=0x%llX 被 %d 个 Actor 共用", (unsigned long long)bs, same);
        }
    }

    LARGE_INTEGER t2;
    QueryPerformanceCounter(&t2);

    // ── 第二遍：容器反查（默认关闭）──
    // 第一遍约 20 秒，第二遍再 10 秒。上一版累计扫到 40 秒左右游戏就消失了，
    // 所以这里默认只交第一遍的结果 —— 有 Actor 列表就已经能做 ESP/KillAura，
    // 容器是优化项，不值得为它冒第二次全堆遍历的风险。
    // 要反查容器时，往 C:\Xiaoyu\deep.txt 放个空文件再重新注入即可。
    const bool deep = GetFileAttributesW(L"C:\\Xiaoyu\\deep.txt") != INVALID_FILE_ATTRIBUTES;
    P("");
    P("第二遍（容器反查）: %s", deep ? "开启" : "跳过（未发现 C:\\Xiaoyu\\deep.txt）");

    if (g_actor_count > 0 && deep) {
        HashBuild();
        g_ref_count = 0;
        g_ref_overflow = false;
        ForEachWritableRegion([](uintptr_t rbase, size_t rsize) {
            PassRefs(rbase, rsize);
        });
        SortRefs();

        P("");
        P("第二遍：%d 处引用%s  (%.0f ms)",
          g_ref_count, g_ref_overflow ? "（已截断）" : "", MsSince(t2));

        P("");
        P("=== REF LIST（按地址升序；相邻 = 同一容器）===");
        for (int i = 0; i < g_ref_count; ++i) {
            const bool contiguous = (i > 0) && (g_refs[i].at - g_refs[i - 1].at == 8);
            P("at=0x%llX actor=0x%llX%s",
              (unsigned long long)g_refs[i].at,
              (unsigned long long)g_refs[i].value,
              contiguous ? "  <<连续>>" : "");
        }

        // 把连续段摘出来单独列一遍，一眼看出哪个是实体数组
        P("");
        P("=== 连续段（同一容器最可能在这里）===");
        int i = 0;
        while (i < g_ref_count) {
            int j = i;
            while (j + 1 < g_ref_count && g_refs[j + 1].at - g_refs[j].at == 8) ++j;
            const int n = j - i + 1;
            if (n >= 2) {
                P("  0x%llX 起连续 %d 个   跨度 0x%llX ~ 0x%llX",
                  (unsigned long long)g_refs[i].at, n,
                  (unsigned long long)g_refs[i].at,
                  (unsigned long long)g_refs[j].at);
            }
            i = j + 1;
        }
    }

    P("");
    P("总耗时 %.0f ms", MsSince(t0));
    P("############################################################");

    PClose();
}

void Start(unsigned delay_ms) {
    HANDLE th = CreateThread(nullptr, 0, &ThreadProc, (LPVOID)(uintptr_t)delay_ms, 0, nullptr);
    if (th != nullptr) CloseHandle(th);
}

}  // namespace explorer
}  // namespace xiaoyu
