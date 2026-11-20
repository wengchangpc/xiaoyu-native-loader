// esp.cpp - 实体透视
//
// ── 为什么这么写 ────────────────────────────────────────────────
// 客户端读内存的模块一个都没有，ESP 是第一个。所以这一版的目标不是功能全，
// 而是「把整条链路跑通并且能自己证明自己没错」：
//
//   1. 数据只从 sdk.h 里实测过的偏移来（LocalPlayer 静态全局 -> BlockSource
//      -> Level -> ActorManager -> ECS storage），不猜任何东西。
//   2. 投影自己算（不用游戏矩阵）。摄像机 = LocalPlayer 眼睛位置 + ECS 里的
//      rotation 组件。第一人称视角下这两者和真实摄像机完全一致，
//      第三人称会差一个后撤距离，但方向对，框会跟着人走。
//   3. 每帧把「枚举到几个实体 / 认出几个玩家 / 画出来几个」写进 native.log
//      （限流 3 秒一次）。ESP 出问题几乎都是链路断了，有这三行数字就能定位
//      是 ECS 读不到、还是类名过滤太严、还是投影全在屏幕外。
//
// ── 坐标系约定（Bedrock 的）────────────────────────────────────
//   yaw   0 度 = 朝 +Z，90 度 = 朝 -X，顺时针增长（俯视）
//   pitch 正数 = 低头
//   前方向量  f = (-sin(yaw)cos(pitch), -sin(pitch), cos(yaw)cos(pitch))
//   右方向量  r = (-cos(yaw), 0, -sin(yaw))
//   上方向量  u = r x f = (-sin(yaw)sin(pitch), cos(pitch), cos(yaw)sin(pitch))
//   屏幕投影  sx = W/2 + (d·r) * focal / (d·f)
//            sy = H/2 - (d·u) * focal / (d·f)
//            focal = (H/2) / tan(vfov/2)
//
// ── FOV ────────────────────────────────────────────────────────
// 参数在 C:\Xiaoyu\esp.ini（独立的 ini，不塞进 native_config.txt ——
// 那个文件由 cfg::Save() 全量重写，自定义段会被丢掉）：
//     [esp]
//     fov=70
//     eye_offset=0
// 默认 70（垂直）**只是猜的**。填错的表现很典型：所有框从屏幕中心向外
// 等比放大/缩小 —— 中心附近看着还行，越靠屏幕边缘偏得越多。因为投影
// 中心永远是对的，误差只随偏离中心的角度增长。
// 拿到真值的办法（不用重编译）：连管道发 scanfov，扫 Options 里的 float，
// 再用 fov <数值> 实时覆盖。
//
// ── 几何（2026-11-18 用 AABB 实测校正）────────────────────────
//   mPos（Actor+0xCE0 / StateVectorComponent::mPos）**是眼睛位置**，
//   不是脚底。AABB 的 min.y 才是脚底，且站在地面上时是整数。
//   所以：相机原点 = mPos 直接用；框 = UAABBShapeComponent 的 min/max。
//   两处都用实测值之后，怪物/掉落物/潜行玩家各自画各自的真实碰撞箱。
#include "esp.h"

#include <windows.h>

#include <cstdarg>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "gui/catalog.h"
#include "log.h"
#include "render.h"
#include "sdk.h"

namespace xiaoyu {
namespace esp {
namespace {

const float kPi = 3.14159265358979f;
const float kDeg2Rad = kPi / 180.0f;

// ★ 单独的 ini，不塞进 native_config.txt：
//   那个文件由 cfg::Save() 全量重写，任何一次 GUI 改动都会把自定义的 [esp] 段
//   整段丢掉，fov 会悄无声息退回默认值。独立文件不参与那套存档。
const wchar_t* kIniPath = L"C:\\Xiaoyu\\esp.ini";

int g_last_drawn = 0;
int g_last_actors = 0;

// 运行期覆盖。管道（rpc）改这三个值，调对齐时不用重注入。
// 用 volatile 是因为写它们的是管道线程、读的是覆盖层渲染线程。
// 对齐的 4 字节读写本身就是原子的，不需要额外加锁。
volatile LONG g_force = -1;         // -1 听配置  0 强制关  1 强制开
volatile LONG g_fov_override = 0;   // 0 = 用 esp.ini
volatile float g_eye_override = 0;  // <=0 = 用 esp.ini

// ── 目录查询（和 modules.cpp 里那套一致，不跨文件引用它的匿名命名空间）──
int FindFeature(const wchar_t* en) {
    for (int i = 0; i < kFeatureCount; ++i) {
        if (kFeatures[i].en != nullptr && wcscmp(kFeatures[i].en, en) == 0) return i;
    }
    return -1;
}

float ParamOf(int fi, const wchar_t* attr, float def) {
    if (fi < 0) return def;
    const FeatureInfo& f = kFeatures[fi];
    for (int k = 0; k < f.paramCount; ++k) {
        const int pi = f.paramFirst + k;
        if (kParams[pi].attr != nullptr && wcscmp(kParams[pi].attr, attr) == 0) {
            return cfg::ParamValue(pi, def);
        }
    }
    return def;
}

bool ParamOn(int fi, const wchar_t* attr) {
    return ParamOf(fi, attr, 0.0f) > 0.5f;
}

// 垂直 FOV。以前只读一次，改 ini 得重开一个实例才生效；投影要拿它跟真实
// 画面对齐，反复重注入太慢，所以改成每 2 秒重读一次，改完 native_config.txt
// 两秒内就能在屏幕上看到效果。
int FovDeg() {
    // 管道设过就直接用，绕开 ini 和 2 秒缓存 —— 调对齐时要的是"立刻生效"
    const int ov = (int)g_fov_override;
    if (ov >= 20 && ov <= 140) return ov;

    static int v = 70;
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (last == 0 || now - last > 2000) {
        last = now;
        int n = (int)GetPrivateProfileIntW(L"esp", L"fov", 70, kIniPath);
        if (n < 20 || n > 140) n = 70;
        v = n;
    }
    return v;
}

// 相机原点的**额外偏移**（不是眼睛高度）。
//
// mPos 本身就是眼睛位置，所以这里默认 0。留这个口子是给"第三人称"用的：
// 第三人称时真实相机在后面若干格，方向对但位置差一截，可以拿它做微调。
// 也可以用来验证"mPos 到底是不是眼睛"——把它设成 ±1.62，画面应该整体上下
// 平移而不是缩放；如果观察到的是缩放，说明相机原点取错了。
float EyeOffset() {
    const float ov = g_eye_override;
    if (ov > -3.0f && ov < 3.0f && ov != 0.0f) return ov;

    static float v = 0.0f;
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (last == 0 || now - last > 2000) {
        last = now;
        wchar_t buf[32] = {0};
        GetPrivateProfileStringW(L"esp", L"eye_offset", L"0", buf, 32, kIniPath);
        const float f = (float)_wtof(buf);
        if (f > -3.0f && f < 3.0f) v = f;
    }
    return v;
}

struct Camera {
    sdk::Vec3 eye;
    float fx, fy, fz;   // forward
    float rx, ry, rz;   // right
    float ux, uy, uz;   // up
    float focal;
};

void MakeCamera(const sdk::ActorEntry& me, int w, int h, Camera* c) {
    const float yaw = me.yaw * kDeg2Rad;
    const float pitch = me.pitch * kDeg2Rad;
    const float cy = cosf(yaw), sy = sinf(yaw);
    const float cp = cosf(pitch), sp = sinf(pitch);

    // ★ me.pos 已经是眼睛位置（见 sdk.h 里那段 mPos 反证），不要再加 1.62
    c->eye = me.pos;
    c->eye.y += EyeOffset();

    c->fx = -sy * cp;
    c->fy = -sp;
    c->fz = cy * cp;

    c->rx = -cy;
    c->ry = 0.0f;
    c->rz = -sy;

    c->ux = -sy * sp;
    c->uy = cp;
    c->uz = cy * sp;

    const float vfov = (float)FovDeg() * kDeg2Rad;
    c->focal = ((float)h * 0.5f) / tanf(vfov * 0.5f);
    (void)w;
}

// 优先用游戏自己的摄像机矩阵（见 sdk.h ReadGameCamera 的注释：0x4E0 旋转、
// 0x520 投影、0x560 相机位置）。
//
// 为什么必须换：老路径要从 Actor 的 rotation 段拿 yaw/pitch 再自己拼相机，
// 而 probe34 实测这个 build 上 Actor 前 24KB 里根本没有跟着视角变的字段，
// 老路径拿到的是 0 —— 拼出来的相机永远是朝 +z 的，画出来必然全错。
// 用游戏矩阵还有个额外好处：FOV、第三人称、镜头晃动、摔伤抖动全都自动一致。
bool MakeCameraFromGame(int w, int h, Camera* c) {
    const uintptr_t lp = sdk::LocalPlayer();
    if (lp == 0) return false;
    const uintptr_t ci = sdk::ClientInstanceOf(lp);
    if (ci == 0) return false;
    sdk::GameCamera gc;
    if (!sdk::ReadGameCamera(ci, &gc)) return false;
    c->eye = gc.eye;
    c->fx = gc.fx; c->fy = gc.fy; c->fz = gc.fz;
    c->rx = gc.rx; c->ry = gc.ry; c->rz = gc.rz;
    c->ux = gc.ux; c->uy = gc.uy; c->uz = gc.uz;
    c->focal = ((float)h * 0.5f) * gc.p11;
    (void)w;
    return true;
}

// 扫描兜底：ECS 链路在这个 build 上失效（见 sdk.h ScanActors 上面那段），
// 实体列表由后台扫描线程维护，这里只取快照 + 每帧现读坐标。
// 所以「谁是实体」最多滞后 1.5 秒，坐标永远是当前值。
int ActorsFromScan(sdk::ActorEntry* out, int max) {
    static uintptr_t addrs[512];
    sdk::ActorSetStart();
    const int n = sdk::ActorSetSnapshot(addrs, 512);
    int got = 0;
    for (int i = 0; i < n && got < max; ++i) {
        if (addrs[i] == 0) continue;
        sdk::ActorEntry e;
        e.actor = addrs[i];
        e.entity_id = 0;
        e.pitch = 0.0f;
        e.yaw = 0.0f;
        e.bb_min.x = 0.0f; e.bb_min.y = 0.0f; e.bb_min.z = 0.0f;
        e.bb_max.x = 0.0f; e.bb_max.y = 0.0f; e.bb_max.z = 0.0f;
        e.has_bb = false;
        if (!mem::Read(e.actor + sdk::kOffPos, &e.pos, sizeof(e.pos))) continue;
        if (!sdk::PlausibleActorPos(e.pos)) continue;
        out[got++] = e;
    }
    return got;
}

// 世界点 -> 屏幕点。返回 false 表示在摄像机后面或者太近，不要画。
bool Project(const Camera& c, float x, float y, float z, float w, float h,
             float* sx, float* sy) {
    const float dx = x - c.eye.x;
    const float dy = y - c.eye.y;
    const float dz = z - c.eye.z;

    const float cz = dx * c.fx + dy * c.fy + dz * c.fz;
    if (cz < 0.08f) return false;

    const float cx = dx * c.rx + dy * c.ry + dz * c.rz;
    const float cy = dx * c.ux + dy * c.uy + dz * c.uz;

    const float inv = c.focal / cz;
    *sx = w * 0.5f + cx * inv;
    *sy = h * 0.5f - cy * inv;
    return true;
}

void HsvToRgb(float hh, float s, float v, float* r, float* g, float* b) {
    hh = fmodf(hh, 1.0f);
    if (hh < 0.0f) hh += 1.0f;
    const float i = floorf(hh * 6.0f);
    const float f = hh * 6.0f - i;
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - f * s);
    const float t = v * (1.0f - (1.0f - f) * s);
    switch (((int)i) % 6) {
        case 0: *r = v; *g = t; *b = p; break;
        case 1: *r = q; *g = v; *b = p; break;
        case 2: *r = p; *g = v; *b = t; break;
        case 3: *r = p; *g = q; *b = v; break;
        case 4: *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

// 拉一个 8 角点的屏幕包围盒。任何一角跑到摄像机后面就先不画（会画出
// 一条几万像素的斜线，比不画更干扰）。
//
// 包围盒直接来自 UAABBShapeComponent —— 这才是实体真正的碰撞箱，
// 怪物/掉落物/潜行玩家各不相同，写死 0.6×1.8 只对站着的玩家成立。
bool BoxOf(const Camera& cam, const sdk::ActorEntry& e, float w, float h,
           float* x0, float* y0, float* x1, float* y1) {
    sdk::Vec3 lo, hi;
    if (e.has_bb) {
        lo = e.bb_min;
        hi = e.bb_max;
    } else {
        // 兜底：按玩家尺寸推。★ pos 是眼睛，脚底要减掉眼高。
        const float hw = sdk::kPlayerWidth * 0.5f;
        const float feet = e.pos.y - sdk::kEyeHeight;
        lo.x = e.pos.x - hw; lo.y = feet;                       lo.z = e.pos.z - hw;
        hi.x = e.pos.x + hw; hi.y = feet + sdk::kPlayerHeight;  hi.z = e.pos.z + hw;
    }

    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < 8; ++i) {
        const float px = (i & 1) ? hi.x : lo.x;
        const float py = (i & 2) ? hi.y : lo.y;
        const float pz = (i & 4) ? hi.z : lo.z;
        float sx = 0.0f, sy = 0.0f;
        if (!Project(cam, px, py, pz, w, h, &sx, &sy)) {
            return false;
        }
        if (sx < minx) minx = sx;
        if (sx > maxx) maxx = sx;
        if (sy < miny) miny = sy;
        if (sy > maxy) maxy = sy;
    }
    // 极端视角下包围盒会退化成一条线，给个下限免得看见一个点
    if (maxx - minx < 2.0f) { minx -= 1.0f; maxx += 1.0f; }
    if (maxy - miny < 6.0f) { miny -= 3.0f; maxy += 3.0f; }
    *x0 = minx; *y0 = miny; *x1 = maxx; *y1 = maxy;
    return true;
}

void DrawBox(float x0, float y0, float x1, float y1, float t, const D2D1_COLOR_F& c) {
    render::Line(x0, y0, x1, y0, t, c);
    render::Line(x1, y0, x1, y1, t, c);
    render::Line(x1, y1, x0, y1, t, c);
    render::Line(x0, y1, x0, y0, t, c);
}

// ── 3D 线框 ─────────────────────────────────────────────────────
//
// 2D 框只是一个屏幕包围盒：抬头看人时它变成一个越来越扁的矩形，
// 和真实形体完全脱节。3D 模式把 8 个角分别投影再连棱，透视是对的，
// 远处的人框会自然收窄 —— 这也是原版 esp_mode 里 "3d"/"both" 的含义。
//
// 顶点编号用 bit 拼：bit0 = x(hi/lo)，bit1 = y，bit2 = z。
// 于是「相邻顶点」就等价于「编号只差一个 bit」，12 条棱不用手写表。
struct Corners {
    float x[8];
    float y[8];
    bool ok[8];    // 该角在摄像机前方（Project 失败表示在背面）
    bool any;
};

void FillLoHi(const sdk::ActorEntry& e, sdk::Vec3* lo, sdk::Vec3* hi) {
    if (e.has_bb) {
        *lo = e.bb_min;
        *hi = e.bb_max;
        return;
    }
    // 兜底：按玩家尺寸推。pos 是眼睛，脚底要减掉眼高。
    const float hw = sdk::kPlayerWidth * 0.5f;
    const float feet = e.pos.y - sdk::kEyeHeight;
    lo->x = e.pos.x - hw; lo->y = feet;                       lo->z = e.pos.z - hw;
    hi->x = e.pos.x + hw; hi->y = feet + sdk::kPlayerHeight;   hi->z = e.pos.z + hw;
}

void BoxCorners(const Camera& cam, const sdk::ActorEntry& e, float w, float h,
                Corners* out) {
    sdk::Vec3 lo, hi;
    FillLoHi(e, &lo, &hi);
    out->any = false;
    for (int i = 0; i < 8; ++i) {
        const float px = (i & 1) ? hi.x : lo.x;
        const float py = (i & 2) ? hi.y : lo.y;
        const float pz = (i & 4) ? hi.z : lo.z;
        out->ok[i] = Project(cam, px, py, pz, w, h, &out->x[i], &out->y[i]);
        if (out->ok[i]) out->any = true;
    }
}

void Draw3DBox(const Corners& c, float t, const D2D1_COLOR_F& col) {
    for (int a = 0; a < 8; ++a) {
        for (int b = a + 1; b < 8; ++b) {
            const int d = a ^ b;
            if (d != 1 && d != 2 && d != 4) continue;   // 不是相邻顶点，没有棱
            // 有一端在摄像机后面就连一条几万像素的斜线，比不画更恶心
            if (!c.ok[a] || !c.ok[b]) continue;
            render::Line(c.x[a], c.y[a], c.x[b], c.y[b], t, col);
        }
    }
}

// 从 3D 角的投影里反算屏幕包围盒，给文字定位用。
bool BoundsOf(const Corners& c, float* x0, float* y0, float* x1, float* y1) {
    if (!c.any) return false;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < 8; ++i) {
        if (!c.ok[i]) continue;
        if (c.x[i] < minx) minx = c.x[i];
        if (c.x[i] > maxx) maxx = c.x[i];
        if (c.y[i] < miny) miny = c.y[i];
        if (c.y[i] > maxy) maxy = c.y[i];
    }
    if (maxx - minx < 2.0f) { minx -= 1.0f; maxx += 1.0f; }
    if (maxy - miny < 6.0f) { miny -= 3.0f; maxy += 3.0f; }
    *x0 = minx; *y0 = miny; *x1 = maxx; *y1 = maxy;
    return true;
}

// 提前返回时留一句话。上一次 ESP 一根框都不出、日志里一个字都没有，
// 只能靠猜是"模块没开"还是"链路断了"。这里把每种早退原因都标出来，
// 限流 2 秒一次，正常运行时不会刷屏（因为正常路径走不到这里）。
void Why(const char* fmt, ...) {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 2000) return;
    last = now;
    char buf[224];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LogLine("[esp] 停在：%s", buf);
}

}  // namespace

int LastDrawn() { return g_last_drawn; }
int LastActors() { return g_last_actors; }

void SetForce(int v) { InterlockedExchange(&g_force, (LONG)v); }
int Force() { return (int)InterlockedCompareExchange(&g_force, 0, 0); }

void SetFovOverride(int deg) { InterlockedExchange(&g_fov_override, (LONG)deg); }
int FovOverride() { return (int)InterlockedCompareExchange(&g_fov_override, 0, 0); }

void SetEyeOverride(float h) { g_eye_override = h; }
float EyeOverride() { return g_eye_override; }

void Render() {
    // ★ 构建匹配自检挪到「ESP 开关确认打开」之后（见下面）。
    //   以前放在函数最前面，跟开关无关地每帧调 sdk::LocalPlayer()。
    //   现在 LocalPlayer() 在硬编码偏移失效时会拉起一次**全堆扫描**（几 GB，
    //   几秒），放在这里等于用户没开 ESP 也被扫一遍，白白卡游戏。

    const int fi = FindFeature(L"ESP");
    if (fi < 0) {
        Why("目录里找不到 ESP 这一项");
        return;
    }
    // 一次性诊断：把索引和开关值打出来。ESP 不出框时这一行能立刻分清
    // 是"配置没读进来"还是"渲染链路断了"，省掉一轮重新编译。
    static bool s_diag = false;
    if (!s_diag) {
        s_diag = true;
        char nm[40] = {0};
        for (int k = 0; k < 39 && kFeatures[fi].en != nullptr &&
                        kFeatures[fi].en[k] != L'\0'; ++k) {
            nm[k] = (char)kFeatures[fi].en[k];
        }
        const int ac = FindFeature(L"AC");
        LogLine("[esp] diag fi=%d en=%s on=%d force=%d | AC=%d on=%d | kFeatureCount=%d",
                fi, nm, cfg::ModuleOn(fi) ? 1 : 0, (int)g_force, ac,
                (ac >= 0 && cfg::ModuleOn(ac)) ? 1 : 0, kFeatureCount);
    }

    // 管道可以强制开关：调对齐时要频繁 A/B，改配置得重注入，太慢。
    const int force = (int)g_force;
    if (force == 0) {
        g_last_drawn = 0;
        return;
    }
    if (force != 1 && !cfg::ModuleOn(fi)) {
        g_last_drawn = 0;
        Why("模块开关是关的（配置里写 m.ESP=1，或用管道 esp 1 强制打开）");
        return;
    }

    // 构建匹配自检：只在 ESP 真正要干活的时候跑一次。
    // 换 build 之后第一件事就是看 native.log 里这几行 —— 任何 MISMATCH
    // 都说明这份 DLL 不是这个客户端的，后面所有偏移都不可信。
    {
        static bool s_chain_done = false;
        if (!s_chain_done) {
            const uintptr_t lp0 = sdk::LocalPlayer();
            if (lp0 != 0) {
                s_chain_done = true;
                sdk::ChainReport("esp");
            }
        }
    }

    const uintptr_t lp = sdk::LocalPlayer();
    if (lp == 0) {
        g_last_drawn = 0;
        g_last_actors = 0;
        Why("LocalPlayer=0（base=0x%llX，没进世界或静态全局还没写）",
            (unsigned long long)mem::Base());
        return;
    }

    static sdk::ActorEntry actors[512];
    int n = sdk::EnumActors(actors, 512, lp);
    if (n == 0) {
        // ECS 链路失效时（本 build 就是这种情况）走后台扫描器的快照
        n = ActorsFromScan(actors, 512);
    }
    g_last_actors = n;
    if (n == 0) {
        g_last_drawn = 0;
        Why("实体列表是空的（ECS 链路失效，扫描器也还没出结果）");
        return;
    }

    // 找到自己（摄像机）。找不到说明 rotation 段读失败，这一帧只更新计数。
    const sdk::ActorEntry* me = nullptr;
    for (int i = 0; i < n; ++i) {
        if (actors[i].actor == lp) { me = &actors[i]; break; }
    }
    if (me == nullptr) {
        g_last_drawn = 0;
        Why("枚举到 %d 个实体，但里面没有 LocalPlayer", n);
        return;
    }

    const int w = render::Width();
    const int h = render::Height();
    if (w <= 0 || h <= 0) return;

    Camera cam;
    if (!MakeCameraFromGame(w, h, &cam)) {
        // 读不到游戏矩阵才退回 yaw/pitch 版本（本 build 上这个分支大概率出不来）
        MakeCamera(*me, w, h, &cam);
    }

    // ── 样式参数 ──
    const float max_dist = ParamOf(fi, L"scan_radius", 64.0f);
    // 0=2d 1=3d 2=both
    const int mode = (int)ParamOf(fi, L"esp_mode", 0.0f);
    const bool rainbow = ParamOn(fi, L"rainbow");
    const float rspd = ParamOf(fi, L"rainbow_speed", 12.0f);
    float cr = ParamOf(fi, L"color_r", 1.0f);
    float cg = ParamOf(fi, L"color_g", 0.25f);
    float cb = ParamOf(fi, L"color_b", 0.25f);
    if (rainbow) {
        const float t = (float)(GetTickCount64() % 100000ULL) / 1000.0f;
        HsvToRgb(t * rspd / 60.0f, 0.85f, 1.0f, &cr, &cg, &cb);
    }
    const D2D1_COLOR_F box = render::RgbaF(cr, cg, cb, 0.95f);
    const D2D1_COLOR_F text = render::Rgba(255, 255, 255, 235);
    const D2D1_COLOR_F text_dim = render::Rgba(210, 210, 210, 190);

    int drawn = 0;
    // 最近的那个玩家单独记一份：调投影对齐的时候，唯一有意义的对照就是
    // 「日志里算出来的框」vs「截图里人身上真实的框」，两个数字并排放才能解方程。
    float near_dist = 1e9f;
    float nb[4] = {0, 0, 0, 0};
    float near_bb[3] = {0, 0, 0};
    sdk::Vec3 near_pos = {0, 0, 0};
    int near_idx = -1;

    for (int i = 0; i < n; ++i) {
        const sdk::ActorEntry& e = actors[i];
        if (e.actor == lp) continue;

        const char* cls = sdk::ClassNameOf(e.actor);
        if (!sdk::IsPlayerClass(cls)) continue;

        const float dx = e.pos.x - me->pos.x;
        const float dy = e.pos.y - me->pos.y;
        const float dz = e.pos.z - me->pos.z;
        const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dist > max_dist) continue;

        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        Corners cor;
        // esp_mode：0=2d 1=3d 2=both（和原版枚举一致）
        const bool want_2d = (mode != 1);
        const bool want_3d = (mode != 0);
        bool box_ok = false;
        if (want_2d) box_ok = BoxOf(cam, e, (float)w, (float)h, &x0, &y0, &x1, &y1);
        if (want_3d) {
            BoxCorners(cam, e, (float)w, (float)h, &cor);
            if (!box_ok && cor.any) box_ok = BoundsOf(cor, &x0, &y0, &x1, &y1);
        }
        if (!box_ok) continue;

        // 完全在屏幕外就跳过（留一点余量，避免边缘闪烁）
        if (x1 < -64.0f || x0 > (float)w + 64.0f) continue;
        if (y1 < -64.0f || y0 > (float)h + 64.0f) continue;

        if (want_2d) DrawBox(x0, y0, x1, y1, 1.6f, box);
        if (want_3d) Draw3DBox(cor, 1.4f, box);

        // 名字：类名去掉 Player 后缀太啰嗦，直接原样显示
        wchar_t name[48];
        int k = 0;
        for (; cls != nullptr && cls[k] != '\0' && k < 40; ++k) name[k] = (wchar_t)cls[k];
        name[k] = L'\0';
        if (k > 0) {
            render::Text(name, x0, y0 - 15.0f, x1 - x0, 14.0f, 12.5f, text,
                         render::ALIGN_CENTER, false);
        }

        wchar_t info[48];
        swprintf_s(info, L"%.1fm", dist);
        render::Text(info, x0, y1 + 1.0f, x1 - x0, 14.0f, 12.0f, text_dim,
                     render::ALIGN_CENTER, false);

        ++drawn;
        if (dist < near_dist) {
            near_dist = dist;
            nb[0] = x0; nb[1] = y0; nb[2] = x1; nb[3] = y1;
            near_bb[0] = e.has_bb ? (e.bb_max.x - e.bb_min.x) : 0.0f;
            near_bb[1] = e.has_bb ? (e.bb_max.y - e.bb_min.y) : 0.0f;
            near_bb[2] = e.has_bb ? (e.bb_max.z - e.bb_min.z) : 0.0f;
            near_pos = e.pos;
            near_idx = i;
        }
    }
    g_last_drawn = drawn;

    // bb = 有多少个实体读到了可信的 AABB。这个数字明显小于 actors 就说明
    // AABB 段的下标没对齐（或者 storage 换了偏移），框会退回写死的 0.6×1.8。
    int with_bb = 0;
    for (int i = 0; i < n; ++i) {
        if (actors[i].has_bb) ++with_bb;
    }

    // 限流日志：ESP 不出框的时候，这三行数字能直接定位是哪一段断了。
    //
    // ★ yaw/pitch 必须从相机朝向反算，不能打 me->yaw。ECS 链路失效后实体
    //   是从扫描表构造的，ActorsFromScan 里 yaw/pitch 是写死的 0，直接打
    //   永远是 0.0 —— 看着像「相机丢了」，实际只是日志字段没接上。
    const float cam_yaw = atan2f(-cam.fx, cam.fz) * 57.29578f;
    const float cam_pitch = -asinf(cam.fy > 1.0f ? 1.0f : (cam.fy < -1.0f ? -1.0f : cam.fy)) * 57.29578f;
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last > 3000) {
        last = now;
        LogLine("[esp] actors=%d bb=%d drawn=%d eye=(%.2f,%.2f,%.2f) yaw=%.1f pitch=%.1f "
                "fov=%d off=%.2f focal=%.1f screen=%dx%d",
                n, with_bb, drawn, me->pos.x, me->pos.y, me->pos.z, cam_yaw, cam_pitch,
                FovDeg(), EyeOffset(), cam.focal, w, h);
        if (near_idx >= 0) {
            LogLine("[esp] 最近体 #%d cls=%s pos=(%.2f,%.2f,%.2f) d=%.2f "
                    "bb=%.2fx%.2fx%.2f box=(%.1f,%.1f)-(%.1f,%.1f) %.0fx%.0f",
                    near_idx, sdk::ClassNameOf(actors[near_idx].actor),
                    near_pos.x, near_pos.y, near_pos.z, near_dist,
                    near_bb[0], near_bb[1], near_bb[2],
                    nb[0], nb[1], nb[2], nb[3], nb[2] - nb[0], nb[3] - nb[1]);
        }
    }
}

// ── 附加渲染：BorderRender / LineRender / ParticleTrail ─────────
//
// 这三个都是「只画不读」的纯本地渲染：不碰游戏渲染管线、不改内存、不发包，
// 只是把已经拿到的世界坐标投到屏幕上再画一遍。反作弊侧完全无感。
//
// 为什么要独立入口，不塞进 Render()：
//   Render() 在 ESP 开关关闭时会提前 return，而这三个功能是各自独立的开关。
//   塞进去的话「只开 LineRender 不开 ESP」就什么都画不出来。
//
// 相机和实体都不重复算：MakeCameraFromGame 每次只读 3 个字段（矩阵/投影/位置），
// ActorsFromScan 读的是后台扫描器的快照 + 每个实体一次 read，成本跟 ST 一样。

struct TrailPt {
    sdk::Vec3 p;
    ULONGLONG t;
};

const int kTrailMax = 96;
TrailPt g_trail[kTrailMax];
int g_trail_head = 0;      // 下一个写入位置
int g_trail_fill = 0;      // 已填充个数
ULONGLONG g_trail_last = 0;

// 玩家身后拖尾。采样的是**玩家自己的位置**，每隔 interval 存一个点，
// 画的时候按年龄衰减透明度 —— 所以 interval 小 = 点密、拖尾长；
// count 决定每个采样点画几个抖动点（视觉上厚一点）。
void DrawTrail(int trail_fi, const sdk::Vec3& me_pos, const Camera& cam, int w, int h) {
    const ULONGLONG now = GetTickCount64();
    float iv = ParamOf(trail_fi, L"interval", 0.15f);
    if (iv < 0.05f) iv = 0.05f;
    if (iv > 2.0f) iv = 2.0f;
    const ULONGLONG iv_ms = (ULONGLONG)(iv * 1000.0f);
    if (g_trail_last == 0 || now - g_trail_last >= iv_ms) {
        g_trail_last = now;
        g_trail[g_trail_head].p = me_pos;
        g_trail[g_trail_head].t = now;
        g_trail_head = (g_trail_head + 1) % kTrailMax;
        if (g_trail_fill < kTrailMax) ++g_trail_fill;
    }

    int cnt = (int)ParamOf(trail_fi, L"count", 1.0f);
    if (cnt < 1) cnt = 1;
    if (cnt > 20) cnt = 20;

    for (int i = 0; i < g_trail_fill; ++i) {
        const int idx = (g_trail_head - 1 - i + kTrailMax * 2) % kTrailMax;
        const TrailPt& tp = g_trail[idx];
        if (now < tp.t) continue;
        const ULONGLONG age = now - tp.t;
        if (age > 6000) break;                 // 更老的都过期了，环里是按时间倒序排的
        const float a = 1.0f - (float)age / 6000.0f;
        for (int k = 0; k < cnt; ++k) {
            // 抖动用固定相位，别用 rand：同一帧内多次调用要稳定，别闪
            const float ph = (float)((i * 7 + k * 13) % 17) / 17.0f * 6.28318f;
            const float ox = cosf(ph) * 0.22f;
            const float oz = sinf(ph) * 0.22f;
            float sx = 0, sy = 0;
            if (!Project(cam, tp.p.x + ox, tp.p.y + 0.9f, tp.p.z + oz,
                         (float)w, (float)h, &sx, &sy)) {
                continue;
            }
            const float rad = 3.0f + 2.0f * a;
            render::Rect(sx - rad, sy - rad, rad * 2.0f, rad * 2.0f,
                         render::RgbaF(0.45f, 0.85f, 1.0f, a * 0.55f));
        }
    }
}

void RenderExtras() {
    const int border_fi = FindFeature(L"BorderRender");
    const int line_fi = FindFeature(L"LineRender");
    const int trail_fi = FindFeature(L"ParticleTrail");
    const bool want_border = (border_fi >= 0) && cfg::ModuleOn(border_fi);
    const bool want_line = (line_fi >= 0) && cfg::ModuleOn(line_fi);
    const bool want_trail = (trail_fi >= 0) && cfg::ModuleOn(trail_fi);
    if (!want_border && !want_line && !want_trail) {
        // 关掉之后把环清空，下次打开不会拖出一条几秒前的旧轨迹
        g_trail_fill = 0;
        g_trail_head = 0;
        g_trail_last = 0;
        return;
    }

    const int w = render::Width();
    const int h = render::Height();
    if (w <= 0 || h <= 0) return;

    const uintptr_t lp = sdk::LocalPlayer();
    if (lp == 0) return;

    Camera cam;
    if (!MakeCameraFromGame(w, h, &cam)) return;

    sdk::Vec3 me_pos = {0, 0, 0};
    const bool have_me = mem::Read(lp + sdk::kOffPos, &me_pos, sizeof(me_pos));

    if (want_trail && have_me) DrawTrail(trail_fi, me_pos, cam, w, h);

    if (!want_border && !want_line) return;

    static sdk::ActorEntry actors[512];
    const int n = ActorsFromScan(actors, 512);
    if (n <= 0) return;

    const float b_rad = ParamOf(border_fi, L"radius", 48.0f);
    const float l_rad = ParamOf(line_fi, L"radius", 32.0f);
    const float l_up = ParamOf(line_fi, L"origin_above", 2.4f);

    // 起点：玩家位置上方 origin_above（原版这个参数是 2.0..3.6，就是身高的量级）
    float ox0 = 0, oy0 = 0;
    const bool have_origin = have_me &&
                             Project(cam, me_pos.x, me_pos.y + l_up, me_pos.z,
                                     (float)w, (float)h, &ox0, &oy0);

    const D2D1_COLOR_F b_col = render::Rgba(120, 200, 255, 200);
    const D2D1_COLOR_F l_col = render::RgbaF(1.0f, 0.62f, 0.2f, 0.45f);

    int nb = 0, nl = 0;
    for (int i = 0; i < n; ++i) {
        const sdk::ActorEntry& e = actors[i];
        if (e.actor == lp) continue;

        const float dx = e.pos.x - cam.eye.x;
        const float dy = e.pos.y - cam.eye.y;
        const float dz = e.pos.z - cam.eye.z;
        const float dist = sqrtf(dx * dx + dy * dy + dz * dz);

        // BorderRender：半径内**所有**实体都框（不分玩家/生物），这是它和 ESP 的区别
        if (want_border && dist <= b_rad) {
            float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            if (BoxOf(cam, e, (float)w, (float)h, &x0, &y0, &x1, &y1)) {
                if (!(x1 < -64.0f || x0 > (float)w + 64.0f ||
                      y1 < -64.0f || y0 > (float)h + 64.0f)) {
                    DrawBox(x0, y0, x1, y1, 1.3f, b_col);
                    ++nb;
                }
            }
        }

        // LineRender：从头顶拉到半径内每个实体
        if (want_line && have_origin && dist <= l_rad) {
            float tx = 0, ty = 0;
            if (Project(cam, e.pos.x, e.pos.y + 0.9f, e.pos.z,
                        (float)w, (float)h, &tx, &ty)) {
                render::Line(ox0, oy0, tx, ty, 1.2f, l_col);
                ++nl;
            }
        }
    }

    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last > 5000) {
        last = now;
        LogLine("[vis] border=%d/%d line=%d/%d trail=%d actors=%d",
                nb, (int)b_rad, nl, (int)l_rad, g_trail_fill, n);
    }
}

}  // namespace esp
}  // namespace xiaoyu
