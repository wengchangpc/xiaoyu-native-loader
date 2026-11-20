// rpc.cpp - 命名管道命令服务 + 脚本绘制列表
//
// 一条命令一行，应答统一是 "OK <len>\n<负载>" 或 "ERR <原因>\n"。
// 之所以用字节长度而不是行结束符：read 要能返回任意二进制，里面必然有 \n。
//
// 为什么要这个（而不是继续用 run.bat）：
//   run.bat 是"放文件 -> 轮询 -> 跑子进程 -> 读日志"，一轮几秒钟，
//   调一个参数要等半天。排查 ESP 对齐的时候需要的是"立刻问一句、立刻改一下"。
//   而且脚本通过这里的 read 命令拿数据，实际读内存的是 DLL 自己 ——
//   外部进程反复 ReadProcessMemory 会被反作弊杀掉，这条路绕开了那个特征。
#include "xyrpc.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "esp.h"
#include "log.h"
#include "memory.h"
#include "render.h"
#include "self.h"
#include "sdk.h"

namespace xiaoyu {
namespace rpc {
namespace {

const wchar_t* kPipeName = L"\\\\.\\pipe\\xiaoyu_rpc";

volatile LONG g_stop = 0;
volatile LONG g_clients = 0;
HANDLE g_thread = nullptr;

// ── 脚本绘制列表 ────────────────────────────────────────────────
enum Kind { K_RECT = 0, K_FILL, K_LINE, K_TEXT };

struct Item {
    int kind;
    float p[5];
    unsigned color;      // 0xAARRGGBB
    wchar_t text[96];
};

const int kMaxItems = 768;
Item g_stage[kMaxItems];      // 脚本正在拼的这一帧
int g_stage_n = 0;
Item g_live[kMaxItems];       // 已提交、正在显示的那一帧
int g_live_n = 0;
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;
ULONGLONG g_last_commit = 0;

D2D1_COLOR_F ToColor(unsigned argb) {
    D2D1_COLOR_F c;
    c.a = ((argb >> 24) & 0xFF) / 255.0f;
    c.r = ((argb >> 16) & 0xFF) / 255.0f;
    c.g = ((argb >> 8) & 0xFF) / 255.0f;
    c.b = (argb & 0xFF) / 255.0f;
    if (c.a <= 0.0f) c.a = 1.0f;
    return c;
}

// ── 管道读写 ────────────────────────────────────────────────────
bool WriteAll(HANDLE h, const void* data, int len) {
    const char* p = (const char*)data;
    int left = len;
    while (left > 0) {
        DWORD w = 0;
        if (!WriteFile(h, p, (DWORD)left, &w, nullptr) || w == 0) return false;
        p += w;
        left -= (int)w;
    }
    return true;
}

void SendOk(HANDLE h, const void* data, int len) {
    char hdr[64];
    int n = sprintf_s(hdr, "OK %d\n", len);
    WriteAll(h, hdr, n);
    if (len > 0 && data != nullptr) WriteAll(h, data, len);
}

void SendErr(HANDLE h, const char* msg) {
    char buf[512];
    int n = sprintf_s(buf, "ERR %s\n", msg ? msg : "?");
    WriteAll(h, buf, n);
}

// 逐字节读到 '\n'。控制命令量很小，不值得为吞吐做优化。
int ReadLine(HANDLE h, char* buf, int cap) {
    int n = 0;
    for (;;) {
        char c = 0;
        DWORD got = 0;
        if (!ReadFile(h, &c, 1, &got, nullptr) || got == 0) return -1;
        if (c == '\n') break;
        if (c != '\r' && n < cap - 1) buf[n++] = c;
    }
    buf[n] = '\0';
    return n;
}

char* NextTok(char** p) {
    char* s = *p;
    while (*s == ' ' || *s == '\t') ++s;
    if (*s == '\0') { *p = s; return nullptr; }
    char* start = s;
    while (*s != '\0' && *s != ' ' && *s != '\t') ++s;
    if (*s != '\0') { *s = '\0'; ++s; }
    *p = s;
    return start;
}

int HexToBytes(const char* s, unsigned char* out, int cap) {
    int n = 0;
    while (s[0] != '\0' && s[1] != '\0' && n < cap) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nib(s[0]), lo = nib(s[1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = (unsigned char)((hi << 4) | lo);
        s += 2;
    }
    return n;
}

// ── 命令实现 ────────────────────────────────────────────────────
void CmdInfo(HANDLE h) {
    const uintptr_t lp = sdk::LocalPlayer();
    char buf[1024];
    int n = sprintf_s(buf,
        "pid=%lu\nbase=0x%llX\nsize=0x%llX\nlp=0x%llX\nlevel=0x%llX\n"
        "actorsmgr=0x%llX\nclientinstance=0x%llX\noptions=0x%llX\n"
        "w=%d\nh=%d\n",
        (unsigned long)GetCurrentProcessId(),
        (unsigned long long)mem::Base(),
        (unsigned long long)mem::ImageSize(),
        (unsigned long long)lp,
        (unsigned long long)(lp ? sdk::LevelOf(lp) : 0),
        (unsigned long long)(lp ? sdk::ActorManagerOf(lp) : 0),
        (unsigned long long)(lp ? sdk::ClientInstanceOf(lp) : 0),
        (unsigned long long)(lp ? sdk::OptionsOf(lp) : 0),
        render::Width(), render::Height());
    SendOk(h, buf, n);
}

void CmdActors(HANDLE h) {
    static sdk::ActorEntry list[512];
    const int n = sdk::EnumActors(list, 512);
    std::string out;
    char line[320];
    // 一行一条，纯空格分隔，方便脚本直接 split —— 不要在这里加中文，
    // 解析端是按列取数的。
    //   idx addr class x y z yaw pitch id hasbb minx miny minz maxx maxy maxz
    for (int i = 0; i < n; ++i) {
        const sdk::ActorEntry& e = list[i];
        const char* cls = sdk::ClassNameOf(e.actor);
        sprintf_s(line,
                  "%d 0x%llX %s %.3f %.3f %.3f %.2f %.2f %u %d "
                  "%.3f %.3f %.3f %.3f %.3f %.3f\n",
                  i, (unsigned long long)e.actor, cls ? cls : "?",
                  e.pos.x, e.pos.y, e.pos.z, e.pitch, e.yaw, e.entity_id,
                  e.has_bb ? 1 : 0,
                  e.bb_min.x, e.bb_min.y, e.bb_min.z,
                  e.bb_max.x, e.bb_max.y, e.bb_max.z);
        out += line;
    }
    SendOk(h, out.data(), (int)out.size());
}

// scanfov [hexaddr] [len]
//
// 在一段内存里找落在 30..110 的 float，用来定位 Options 里的 FOV 字段。
// 不带参数就扫 Options 对象前 0x2000 字节。
// 为什么要这个：FOV 一直靠 esp.ini 手填，填错就是"框比人高"这种玄学问题。
// 把真值读出来之后，投影就完全不用猜了。
void CmdScanFov(HANDLE h, char** ctx) {
    char* a = NextTok(ctx);
    uintptr_t addr = 0;
    if (a != nullptr) {
        addr = (uintptr_t)strtoull(a, nullptr, 16);
    } else {
        const uintptr_t lp = sdk::LocalPlayer();
        addr = (lp != 0) ? sdk::OptionsOf(lp) : 0;
    }
    int len = 0x2000;
    char* b = NextTok(ctx);
    if (b != nullptr) {
        const long v = strtol(b, nullptr, 0);
        if (v > 0 && v <= (1 << 20)) len = (int)v;
    }
    if (addr == 0) { SendErr(h, "拿不到 Options（没进世界？）"); return; }

    static unsigned char buf[1 << 20];
    if (!mem::Read(addr, buf, (size_t)len)) { SendErr(h, "读不到这段内存"); return; }

    std::string out;
    char line[96];
    const int nf = len / 4;
    int hits = 0;
    for (int i = 0; i < nf; ++i) {
        float f = 0.0f;
        memcpy(&f, buf + i * 4, 4);
        // NaN 和 Inf 在这里天然被排除（比较结果为 false）
        if (!(f >= 30.0f && f <= 110.0f)) continue;
        sprintf_s(line, "+0x%04X = %.4f\n", i * 4, f);
        out += line;
        ++hits;
    }
    if (hits == 0) out = "（没有落在 30..110 的 float）\n";
    SendOk(h, out.data(), (int)out.size());
}

// ── 全进程内存扫描（诊断用）─────────────────────────────────────
//
// 为什么要放进 DLL：VirtualQuery 只对**自身进程**有效，外部工具想遍历这个
// 进程的堆得 OpenProcess + 自建区段表，句柄和访问模式都很显眼。放在游戏
// 进程里扫，外面只看到一条管道请求。
//
// 用来找 SDK 对象和全局槽位，两步都不需要预先知道任何偏移：
//   1) scan <base+vftableRVA>   对象首 8 字节就是 vftable 绝对地址，
//                               这是个 8 字节常量，堆里命中率极高 → 对象地址
//   2) who  <对象地址>           谁的值等于这个地址 → 持有它的全局槽位
//
// 实测必须这么做：这个 build 的可写节里只有一个 vftable 指针，也就是说
// 所有 SDK 对象都是堆分配的，静态扫 .data 一条都找不到。
void CmdScan(HANDLE h, char** ctx) {
    char* a = NextTok(ctx);
    if (a == nullptr) { SendErr(h, "scan <hexval> [maxHits]"); return; }
    const uint64_t needle = strtoull(a, nullptr, 16);
    int cap = 32;
    if (char* b = NextTok(ctx)) cap = (int)strtol(b, nullptr, 10);
    if (cap < 1) cap = 1;
    if (cap > 128) cap = 128;

    static mem::HeapHit hits[128];
    size_t scanned = 0;
    const int n = mem::ScanHeapQword(&needle, 1, hits, cap, &scanned);

    std::string s;
    char line[128];
    sprintf_s(line, "scanned=0x%llX count=%d\n", (unsigned long long)scanned, n);
    s += line;
    for (int i = 0; i < n; ++i) {
        sprintf_s(line, "0x%llX\n", (unsigned long long)hits[i].addr);
        s += line;
    }
    SendOk(h, s.data(), (int)s.size());
}

// who <hexaddr> [maxHits] —— 在主模块映像里找谁持有这个指针。
// 全局变量一定落在映像的数据段，所以只扫映像就够，比扫全堆快两个数量级。
void CmdWho(HANDLE h, char** ctx) {
    char* a = NextTok(ctx);
    if (a == nullptr) { SendErr(h, "who <hexaddr> [maxHits]"); return; }
    const uint64_t needle = strtoull(a, nullptr, 16);
    int cap = 32;
    if (char* b = NextTok(ctx)) cap = (int)strtol(b, nullptr, 10);
    if (cap < 1) cap = 1;
    if (cap > 128) cap = 128;

    static uintptr_t hits[128];
    const int n = mem::FindAllQword(needle, hits, cap, 0, 0);

    std::string s;
    char line[128];
    sprintf_s(line, "base=0x%llX count=%d\n",
              (unsigned long long)mem::Base(), n);
    s += line;
    for (int i = 0; i < n; ++i) {
        sprintf_s(line, "0x%llX rva=0x%llX\n", (unsigned long long)hits[i],
                  (unsigned long long)(hits[i] - mem::Base()));
        s += line;
    }
    SendOk(h, s.data(), (int)s.size());
}

// scanactor [max] —— 谓词扫堆，找所有「像实体」的对象。
//
// 判据是 sdk::ScanActors 里那两条（首字是主模块 vtable + +0xCE0 是合理坐标）。
// 这是 ECS 链失效时的兜底路径：本 build 上 ActorManager 内部偏移全变了，
// 沿着 EntityRegistry -> EntityContext 的页表再挖一跳的成本远高于直接扫。
//
// 输出一行一个，纯 ASCII，方便脚本 split：
//   0x<addr> <ClassName> vt_rva=0x<RVA> pos=x,y,z [SELF]
void CmdScanActors(HANDLE h, char** ctx) {
    int cap = 512;
    if (char* a = NextTok(ctx)) cap = (int)strtol(a, nullptr, 10);
    if (cap < 1) cap = 1;
    if (cap > 1024) cap = 1024;

    static uintptr_t objs[1024];
    size_t scanned = 0;

    // 参考点：本地玩家的坐标。扫堆要靠它做距离过滤 —— 详见 ScanActors 里
    // 那段注释，没有这个锚的话命中全是坐标形如 (112705.1, 0, 112705.1)
    // 的数组元素，一个实体都认不出来。
    const uintptr_t lp = sdk::LocalPlayer();
    sdk::Vec3 ref;
    const bool have_ref = (lp != 0) && mem::Read(lp + sdk::kOffPos, &ref, sizeof(ref));

    // 60 秒不够：实测扫到 60 秒被时限截断时只走了 490MB（全量 1.2GB），
    // LocalPlayer 所在的区段还没轮到，结果连自己都没扫到。给 180 秒。
    const uint64_t t0 = GetTickCount64();
    const int n = sdk::ScanActors(objs, cap, &scanned, t0 + 180000,
                                  have_ref ? &ref : nullptr);
    const uint64_t ms = GetTickCount64() - t0;

    std::string s;
    char line[256];
    sprintf_s(line, "scanned=0x%llX count=%d ms=%llu ref=%s\n",
              (unsigned long long)scanned, n, (unsigned long long)ms,
              have_ref ? "yes" : "no");
    s += line;

    const uintptr_t base = mem::Base();
    for (int i = 0; i < n; ++i) {
        sdk::Vec3 p;
        if (!mem::Read(objs[i] + sdk::kOffPos, &p, sizeof(p))) continue;
        const char* cls = sdk::ClassNameOf(objs[i]);
        const uintptr_t vt = mem::ReadPtr(objs[i]);
        sprintf_s(line, "0x%llX %s vt_rva=0x%llX pos=%.2f,%.2f,%.2f%s\n",
                  (unsigned long long)objs[i], cls != nullptr ? cls : "?",
                  (unsigned long long)(vt - base),
                  p.x, p.y, p.z,
                  objs[i] == lp ? " SELF" : "");
        s += line;
    }
    SendOk(h, s.data(), (int)s.size());
}

void CmdRead(HANDLE h, char** ctx) {
    char* a = NextTok(ctx);
    char* b = NextTok(ctx);
    if (a == nullptr || b == nullptr) { SendErr(h, "read <hexaddr> <len>"); return; }
    const uintptr_t addr = (uintptr_t)strtoull(a, nullptr, 16);
    int len = (int)strtol(b, nullptr, 10);
    if (len <= 0 || len > (1 << 20)) { SendErr(h, "len 1..1048576"); return; }

    // 分块读，任何一块失败就返回已读到的部分（调用方自己能看出来短了）
    static unsigned char buf[1 << 20];
    int got = 0;
    while (got < len) {
        const int chunk = (len - got) > 65536 ? 65536 : (len - got);
        if (!mem::Read(addr + got, buf + got, (size_t)chunk)) break;
        got += chunk;
    }
    SendOk(h, buf, got);
}

void CmdWrite(HANDLE h, char** ctx) {
    char* a = NextTok(ctx);
    char* b = NextTok(ctx);
    if (a == nullptr || b == nullptr) { SendErr(h, "write <hexaddr> <hexbytes>"); return; }
    const uintptr_t addr = (uintptr_t)strtoull(a, nullptr, 16);
    static unsigned char buf[65536];
    const int n = HexToBytes(b, buf, (int)sizeof(buf));
    if (n <= 0) { SendErr(h, "坏 hex"); return; }
    const bool ok = mem::Write(addr, buf, (size_t)n);
    char msg[64];
    sprintf_s(msg, "n=%d ok=%d", n, ok ? 1 : 0);
    SendOk(h, msg, (int)strlen(msg));
}

void CmdDraw(HANDLE h, char** ctx) {
    char* sub = NextTok(ctx);
    if (sub == nullptr) { SendErr(h, "draw reset|rect|fill|line|text|commit"); return; }

    if (strcmp(sub, "reset") == 0) {
        if (g_lock_ready) EnterCriticalSection(&g_lock);
        g_stage_n = 0;
        if (g_lock_ready) LeaveCriticalSection(&g_lock);
        SendOk(h, "ok", 2);
        return;
    }
    if (strcmp(sub, "commit") == 0) {
        if (g_lock_ready) EnterCriticalSection(&g_lock);
        const int n = g_stage_n < kMaxItems ? g_stage_n : kMaxItems;
        memcpy(g_live, g_stage, sizeof(Item) * n);
        g_live_n = n;
        g_last_commit = GetTickCount64();
        if (g_lock_ready) LeaveCriticalSection(&g_lock);
        char msg[32];
        sprintf_s(msg, "n=%d", n);
        SendOk(h, msg, (int)strlen(msg));
        return;
    }

    if (g_stage_n >= kMaxItems) { SendErr(h, "绘制列表满"); return; }
    Item it;
    ZeroMemory(&it, sizeof(it));

    if (strcmp(sub, "rect") == 0 || strcmp(sub, "fill") == 0) {
        // rect x y w h color [thickness]
        char* t[6] = {0};
        for (int i = 0; i < 6; ++i) t[i] = NextTok(ctx);
        if (t[0] == nullptr || t[5] == nullptr) { SendErr(h, "rect x y w h color [t]"); return; }
        it.kind = (sub[0] == 'f') ? K_FILL : K_RECT;
        it.p[0] = (float)atof(t[0]);
        it.p[1] = (float)atof(t[1]);
        it.p[2] = (float)atof(t[2]);
        it.p[3] = (float)atof(t[3]);
        it.color = (unsigned)strtoul(t[4], nullptr, 16);
        it.p[4] = t[5] ? (float)atof(t[5]) : 1.0f;
    } else if (strcmp(sub, "line") == 0) {
        char* t[7] = {0};
        for (int i = 0; i < 7; ++i) t[i] = NextTok(ctx);
        if (t[0] == nullptr || t[4] == nullptr) { SendErr(h, "line x1 y1 x2 y2 color [t]"); return; }
        it.kind = K_LINE;
        it.p[0] = (float)atof(t[0]);
        it.p[1] = (float)atof(t[1]);
        it.p[2] = (float)atof(t[2]);
        it.p[3] = (float)atof(t[3]);
        it.color = (unsigned)strtoul(t[4], nullptr, 16);
        it.p[4] = t[5] ? (float)atof(t[5]) : 1.0f;
    } else if (strcmp(sub, "text") == 0) {
        // text x y size color <剩余整行>
        char* x = NextTok(ctx);
        char* y = NextTok(ctx);
        char* sz = NextTok(ctx);
        char* col = NextTok(ctx);
        if (x == nullptr || col == nullptr) { SendErr(h, "text x y size color text"); return; }
        it.kind = K_TEXT;
        it.p[0] = (float)atof(x);
        it.p[1] = (float)atof(y);
        it.p[2] = (float)atof(sz);
        it.color = (unsigned)strtoul(col, nullptr, 16);
        char* rest = *ctx;
        while (*rest == ' ' || *rest == '\t') ++rest;
        MultiByteToWideChar(CP_UTF8, 0, rest, -1, it.text, 96);
    } else {
        SendErr(h, "未知 draw 子命令");
        return;
    }

    if (g_lock_ready) EnterCriticalSection(&g_lock);
    g_stage[g_stage_n++] = it;
    if (g_lock_ready) LeaveCriticalSection(&g_lock);
    char msg[32];
    sprintf_s(msg, "n=%d", g_stage_n);
    SendOk(h, msg, (int)strlen(msg));
}

void CmdUnload(HANDLE h, char** ctx) {
    char* mode = NextTok(ctx);
    const bool hard = (mode != nullptr && strcmp(mode, "hard") == 0);
    SendOk(h, "bye", 3);
    self::RequestUnload(hard);
}

void CmdInject(HANDLE h, char** ctx) {
    char* path = NextTok(ctx);
    if (path == nullptr) { SendErr(h, "inject <dll 路径>"); return; }
    wchar_t wpath[MAX_PATH] = {0};
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
    HMODULE m = LoadLibraryW(wpath);
    if (m == nullptr) {
        char msg[96];
        sprintf_s(msg, "LoadLibrary 失败 err=%lu", (unsigned long)GetLastError());
        SendErr(h, msg);
        return;
    }
    char msg[64];
    sprintf_s(msg, "h=0x%llX", (unsigned long long)(uintptr_t)m);
    LogLine("[rpc] injected %s -> 0x%llX", path, (unsigned long long)(uintptr_t)m);
    SendOk(h, msg, (int)strlen(msg));
}

void CmdEsp(HANDLE h, char** ctx) {
    char* v = NextTok(ctx);
    if (v == nullptr) { SendErr(h, "esp <-1|0|1>"); return; }
    esp::SetForce(atoi(v));
    char msg[32];
    sprintf_s(msg, "force=%d", esp::Force());
    SendOk(h, msg, (int)strlen(msg));
}

void CmdFov(HANDLE h, char** ctx) {
    char* v = NextTok(ctx);
    if (v == nullptr) { SendErr(h, "fov <deg>（0 = 用 esp.ini）"); return; }
    esp::SetFovOverride(atoi(v));
    char msg[32];
    sprintf_s(msg, "fov=%d", esp::FovOverride());
    SendOk(h, msg, (int)strlen(msg));
}

void CmdEye(HANDLE h, char** ctx) {
    char* v = NextTok(ctx);
    if (v == nullptr) { SendErr(h, "eye <相机 Y 偏移>（0 = 用 esp.ini 的 eye_offset）"); return; }
    esp::SetEyeOverride((float)atof(v));
    char msg[48];
    sprintf_s(msg, "eye=%.3f", esp::EyeOverride());
    SendOk(h, msg, (int)strlen(msg));
}

// 起一个 python.exe 跑脚本，stdout/stderr 原样收回给调用方。
//
// ★ 这不是"在游戏进程里内嵌解释器" —— 那是下一阶段（挂 python313.dll）。
//   这一版是"游戏进程去启动一个 python 子进程"，权限继承游戏的管理员身份，
//   所以脚本能做的事和 bridge 那套完全一样，但不用落 bat、不用等 500ms 轮询、
//   也不用再从日志文件里捞输出。侦察脚本要改十遍的时候差别很大。
void CmdRunPy(HANDLE h, char** ctx) {
    char* sp = NextTok(ctx);
    if (sp == nullptr) { SendErr(h, "runpy <脚本路径> [参数...]"); return; }
    char* rest = *ctx;
    while (*rest == ' ' || *rest == '\t') ++rest;

    wchar_t wpath[MAX_PATH] = {0};
    MultiByteToWideChar(CP_UTF8, 0, sp, -1, wpath, MAX_PATH);
    if (GetFileAttributesW(wpath) == INVALID_FILE_ATTRIBUTES) {
        SendErr(h, "脚本不存在");
        return;
    }
    wchar_t wargs[1024] = {0};
    if (*rest != '\0') MultiByteToWideChar(CP_UTF8, 0, rest, -1, wargs, 1024);

    wchar_t cmd[2048];
    swprintf_s(cmd, L"python \"%s\" %s", wpath, wargs);

    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) { SendErr(h, "CreatePipe 失败"); return; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    // stdin 必须给个有效的空设备。留 nullptr 的话脚本一 import 到读 stdin
    // 的东西就会直接抛异常，报错信息还跟真正的问题无关。
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    // 打开 NUL 失败时不能把 INVALID_HANDLE_VALUE 塞给子进程，
    // 那会被当成一个有效句柄号传下去，python 一读 stdin 就炸。
    si.hStdInput = (nul == INVALID_HANDLE_VALUE) ? nullptr : nul;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    const BOOL ok = CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        char msg[96];
        sprintf_s(msg, "CreateProcess 失败 err=%lu", (unsigned long)GetLastError());
        SendErr(h, msg);
        return;
    }

    std::string out;
    const size_t kCap = 512 * 1024;   // 别让一个死循环脚本把管道撑爆
    const ULONGLONG deadline = GetTickCount64() + 20000;
    char buf[4096];
    bool timed_out = false;
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD got = 0;
            const DWORD want = avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail;
            if (ReadFile(rd, buf, want, &got, nullptr) && got > 0) {
                out.append(buf, got);
                if (out.size() >= kCap) break;
            }
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            // 进程已经结束，把管道里剩下的读完再走
            for (;;) {
                DWORD g2 = 0;
                if (!ReadFile(rd, buf, (DWORD)sizeof(buf), &g2, nullptr) || g2 == 0) break;
                out.append(buf, g2);
                if (out.size() >= kCap) break;
            }
            break;
        }
        if (GetTickCount64() > deadline) {
            TerminateProcess(pi.hProcess, 1);
            timed_out = true;
            break;
        }
        Sleep(15);
    }

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    char tail[64];
    sprintf_s(tail, "\n[runpy] exit=%lu%s", (unsigned long)code,
              timed_out ? " 超时20s已终止" : "");
    out += tail;
    LogLine("[rpc] runpy %s -> exit=%lu len=%d", sp, (unsigned long)code, (int)out.size());
    SendOk(h, out.data(), (int)out.size());
}

// 返回 false 表示客户端断开
bool Serve(HANDLE h) {
    char line[4096];
    for (;;) {
        const int n = ReadLine(h, line, (int)sizeof(line));
        if (n < 0) return false;
        if (n == 0) continue;

        char* ctx = line;
        char* cmd = NextTok(&ctx);
        if (cmd == nullptr) continue;

        if (strcmp(cmd, "ping") == 0) {
            SendOk(h, "pong", 4);
        } else if (strcmp(cmd, "info") == 0) {
            CmdInfo(h);
        } else if (strcmp(cmd, "actors") == 0) {
            CmdActors(h);
        } else if (strcmp(cmd, "scanfov") == 0) {
            CmdScanFov(h, &ctx);
        } else if (strcmp(cmd, "read") == 0) {
            CmdRead(h, &ctx);
        } else if (strcmp(cmd, "scan") == 0) {
            CmdScan(h, &ctx);
        } else if (strcmp(cmd, "who") == 0) {
            CmdWho(h, &ctx);
        } else if (strcmp(cmd, "scanactor") == 0) {
            CmdScanActors(h, &ctx);
        } else if (strcmp(cmd, "write") == 0) {
            CmdWrite(h, &ctx);
        } else if (strcmp(cmd, "draw") == 0) {
            CmdDraw(h, &ctx);
        } else if (strcmp(cmd, "esp") == 0) {
            CmdEsp(h, &ctx);
        } else if (strcmp(cmd, "fov") == 0) {
            CmdFov(h, &ctx);
        } else if (strcmp(cmd, "eye") == 0) {
            CmdEye(h, &ctx);
        } else if (strcmp(cmd, "runpy") == 0) {
            CmdRunPy(h, &ctx);
        } else if (strcmp(cmd, "inject") == 0) {
            CmdInject(h, &ctx);
        } else if (strcmp(cmd, "unload") == 0) {
            CmdUnload(h, &ctx);
            return false;
        } else {
            SendErr(h, "未知命令（ping/info/actors/scanfov/read/scan/scanactor/who/write/draw/esp/fov/eye/runpy/inject/unload）");
        }
    }
}

DWORD WINAPI ThreadProc(LPVOID param) {
    (void)param;
    LogLine("[rpc] 命令通道就绪 %ls", kPipeName);
    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        HANDLE h = CreateNamedPipeW(
            kPipeName, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            LogLine("[rpc] CreateNamedPipe 失败 err=%lu", (unsigned long)GetLastError());
            Sleep(1000);
            continue;
        }
        const BOOL ok = ConnectNamedPipe(h, nullptr);
        if (!ok && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(h);
            if (InterlockedCompareExchange(&g_stop, 0, 0) != 0) break;
            continue;
        }
        InterlockedIncrement(&g_clients);
        Serve(h);
        InterlockedDecrement(&g_clients);
        FlushFileBuffers(h);
        DisconnectNamedPipe(h);
        CloseHandle(h);
    }
    LogLine("[rpc] 命令通道退出");
    return 0;
}

}  // namespace

int ClientCount() { return (int)InterlockedCompareExchange(&g_clients, 0, 0); }

void Start() {
    if (g_thread != nullptr) return;
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = true;
    }
    g_thread = CreateThread(nullptr, 0, &ThreadProc, nullptr, 0, nullptr);
    if (g_thread == nullptr) {
        LogLine("[rpc] CreateThread 失败 err=%lu", (unsigned long)GetLastError());
    }
}

void Stop() {
    InterlockedExchange(&g_stop, 1);
    // ConnectNamedPipe 是阻塞的，自己连一下把它顶醒
    HANDLE h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (g_thread != nullptr) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

void RenderDrawList() {
    if (!g_lock_ready || g_live_n == 0) return;
    // 脚本两秒没提交就认为它不画了，避免连接断开后留一堆僵尸框
    if (GetTickCount64() - g_last_commit > 2000) return;

    EnterCriticalSection(&g_lock);
    const int n = g_live_n;
    for (int i = 0; i < n; ++i) {
        const Item& it = g_live[i];
        const D2D1_COLOR_F c = ToColor(it.color);
        switch (it.kind) {
            case K_RECT: {
                const float t = it.p[4] > 0.1f ? it.p[4] : 1.0f;
                render::Line(it.p[0], it.p[1], it.p[0] + it.p[2], it.p[1], t, c);
                render::Line(it.p[0] + it.p[2], it.p[1], it.p[0] + it.p[2],
                             it.p[1] + it.p[3], t, c);
                render::Line(it.p[0] + it.p[2], it.p[1] + it.p[3], it.p[0],
                             it.p[1] + it.p[3], t, c);
                render::Line(it.p[0], it.p[1] + it.p[3], it.p[0], it.p[1], t, c);
                break;
            }
            case K_FILL:
                render::Rect(it.p[0], it.p[1], it.p[2], it.p[3], c);
                break;
            case K_LINE: {
                const float t = it.p[4] > 0.1f ? it.p[4] : 1.5f;
                render::Line(it.p[0], it.p[1], it.p[2], it.p[3], t, c);
                break;
            }
            case K_TEXT:
                render::Text(it.text, it.p[0], it.p[1], 900.0f, it.p[2] + 6.0f,
                             it.p[2], c, render::ALIGN_LEFT, false);
                break;
            default:
                break;
        }
    }
    LeaveCriticalSection(&g_lock);
}

}  // namespace rpc
}  // namespace xiaoyu
