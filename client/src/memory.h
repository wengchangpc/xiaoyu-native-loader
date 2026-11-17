// memory.h - 进程内内存访问 + 特征码扫描
//
// 这一层只负责「安全地读到自己进程的内存」，不含任何游戏相关的偏移常量。
// 游戏结构体定位全部走签名扫描（memory_offsets.cpp），这样客户端更新后
// 只要特征码还在，就不用重新逆向。
//
// 为什么不用磁盘上的节表：
//   网易这个客户端加了壳，磁盘 PE 的前 7 个节**没有名字**，`.winlice` 节
//   有 15MB 虚拟空间但磁盘占用 0（运行时才解密）。依赖磁盘节表去算
//   .text 范围会直接扫错地方。
//   改成用 VirtualQuery 遍历自己的地址空间 —— 那是壳解密之后的真实布局。
#pragma once

#include <cstddef>
#include <cstdint>

namespace xiaoyu {
namespace mem {

// ── 初始化 ──────────────────────────────────────────────────────
// 解析主模块范围、枚举可读区域。幂等，重复调用只做一次。
// 失败（拿不到 PE 头等）返回 false，之后所有 Read/Scan 都会返回失败。
bool Init();
bool Ready();

uintptr_t Base();        // 主模块基址（= 磁盘 ImageBase 的运行时重定位结果）
size_t ImageSize();      // SizeOfImage
size_t RegionCount();

// 第 i 个可扫描区域。扫描器内部用；外部调试时也能用来打印布局。
struct Region {
    uintptr_t base;
    size_t size;
    uint32_t protect;
    bool exec;
    bool image;   // true = 属于主模块映像（不是堆/栈/映射文件）
};
bool GetRegion(size_t index, Region* out);

// ── 安全读 ──────────────────────────────────────────────────────
// 全部走 VirtualQuery 预检 + SEH 兜底，读到不可访问地址返回 false 而不是崩。
// 指针链解引用请一律用这几个，不要直接 `*(T*)addr`。
bool Read(uintptr_t addr, void* out, size_t n);
bool Write(uintptr_t addr, const void* src, size_t n);

template <class T>
bool Read(uintptr_t addr, T* out) {
    return Read(addr, static_cast<void*>(out), sizeof(T));
}

// 读指针。失败返回 0（0 在正常地址空间里不会是合法对象）。
uintptr_t ReadPtr(uintptr_t addr);

// 指针链：base -> [off0] -> [off1] -> ...
// 任一步失败返回 0。用来把 ClientInstance->LocalPlayer->... 写成一行。
uintptr_t Follow(uintptr_t base, const int* offsets, int count);

// 地址是否落在主模块映像内（用来判断某个值像不像代码/数据指针）
bool InImage(uintptr_t addr);

// ── 特征码扫描 ──────────────────────────────────────────────────
// pattern 用 IDA 风格：'48 8B 05 ?? ?? ?? ?? 48 85 C0'
//   '??' 或 '?'  = 通配
//   '48' '8B' …   = 十六进制字节（大小写都行）
// 返回**第一个**命中的绝对地址；没命中返回 0。
//
// skip：跳过前 N 个命中（有的特征码前面的引用是别的类的）。
// 只在主模块映像内扫；range 非空时只扫 [start, start+len)。
uintptr_t Scan(const char* pattern, int skip = 0);
uintptr_t ScanRange(uintptr_t start, size_t len, const char* pattern, int skip = 0);

// 带缓存的扫描：同一个 id 第二次调用直接返回上次结果（命中 0 也会缓存，
// 但缓存的 0 会在 RescanAll() 时清掉）。
// 定位模块的初始化代码全用它，避免每帧重复扫 200MB。
uintptr_t ScanCached(const char* id, const char* pattern, int skip = 0);

// 清空所有缓存（切换世界/重载时用）
void ClearScanCache();

// ── 按值扫描 ────────────────────────────────────────────────────
// 找"内存里哪个位置存着这个值"。RTTI 链条全靠它：
//   COL 的 pTypeDescriptor 字段是 4 字节 RVA   -> ScanDword
//   vtable[-1] 是 COL 的 8 字节绝对地址        -> ScanQword
// 比把值转成 pattern 字符串再调 Scan 快，也不用怕格式出错。
uintptr_t ScanDword(uint32_t value, uintptr_t from = 0, int skip = 0);
uintptr_t ScanQword(uint64_t value, uintptr_t from = 0, int skip = 0);

// 按任意字节串扫（找 RTTI 类名这类纯文本时用，省得把字符串拼成
// "43 6C 69 ..." 那种 pattern 字符串）。
uintptr_t ScanBytes(const void* bytes, size_t n, int skip = 0);
uintptr_t ScanText(const char* ascii, int skip = 0);

// 收集所有命中（最多 max 个），返回实际个数。
// 用来枚举"哪些地方引用了这个 vtable"，一次看全而不是一个个 skip。
int FindAllQword(uint64_t value, uintptr_t* out, int max,
                 uintptr_t from = 0, uintptr_t to = 0);

// ── 堆扫描 ──────────────────────────────────────────────────────
// 在**整个进程**的私有可写内存里找值（不只是主模块映像）。
// C++ 对象都在堆上，对象首字段就是 vtable 指针，所以找得到就等于拿到了 this。
//
// 只扫 MEM_PRIVATE + 可写区域：映像本身已经用 Scan 查过，文件映射不可能是
// 堆对象。skip_noaccess 之类的属性检查在内部做，读到不可访问页会跳过。
struct HeapHit {
    uintptr_t addr;
    int index;      // 命中的是 needles[index]
};
int ScanHeapQword(const uint64_t* needles, int needle_count,
                  HeapHit* out, int max_out,
                  size_t* scanned_bytes = nullptr);

// 同上，但按字节特征码扫（找字符串 / 结构体特征时用）
int ScanHeapPattern(const char* pattern, uintptr_t* out, int max_out,
                    size_t* scanned_bytes = nullptr);

// ── RIP 相对寻址 ────────────────────────────────────────────────
// x64 里几乎所有的全局单例都是 `mov rax, [rip+disp32]` 这种形式，
// 命中的地址本身没用，要算出它指向哪。
//
//   at        命中地址（指令开头）
//   instr_len 整条指令长度（如 '48 8B 05 disp32' → 7）
//   disp_off  位移字段在这条指令里的偏移（上面的例子是 3）
//
// 返回绝对地址；不合法返回 0。
uintptr_t ResolveRip(uintptr_t at, int instr_len, int disp_off);

// 命中处是一条 `lea reg, [rip+disp32]` / `mov reg, [rip+disp32]` 时，
// 直接拿到它引用的地址。等价于 ResolveRip(at, len, len-4)。
uintptr_t RipTarget(uintptr_t at, int instr_len);

// ── 校验 ────────────────────────────────────────────────────────
// 内存模块最怕「签名过期还照着旧偏移读」——轻则数值乱掉，重则崩游戏。
// 每次定位出结构体后必须做一次自检，失败就整体停用。
struct Check {
    const char* what;   // 检查项名字，写日志用
    bool ok;
};
bool AddCheck(const char* what, bool ok);
int FailedCheckCount();
const char* FirstFailedCheck();

// ── dump ────────────────────────────────────────────────────────
// 把主模块的运行时内存按 PE 结构写回文件，供离线用 IDA / Ghidra 分析。
//
// 关键点：输出文件的 FileAlignment 直接设成 SectionAlignment(0x1000)，
// 于是 PointerToRawData == VirtualAddress，文件偏移和运行时 RVA 完全对齐，
// 反汇编器里看到的地址就是日志里打印的地址，不用来回换算。
//
// 只写「可读且属于映像」的区域；.winlice 那种 PAGE_NOACCESS 的空洞保留为 0。
bool DumpImage(const wchar_t* path);

// 只 dump 指定范围（拿到某个函数的地址后想单独特看时用）
bool DumpRange(const wchar_t* path, uintptr_t start, size_t len);

// dump 目录：C:\Xiaoyu\dump\
const wchar_t* DumpDir();

}  // namespace mem
}  // namespace xiaoyu
