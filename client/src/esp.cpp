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
// 拿不到游戏里的 FOV 设置，所以先在 C:\Xiaoyu\native_config.txt 里配：
//     [esp]
//     fov=70
// 默认 70（垂直）。配错了不会「框乱飞」，只会让屏幕边缘的框偏一点 ——
// 因为投影中心是对的，误差随偏离屏幕中心的角度增大。
#include "esp.h"

#include <windows.h>

#include <math.h>
#include <stdio.h>
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

const wchar_t* kIniPath = L"C:\\Xiaoyu\\native_config.txt";

int g_last_drawn = 0;
int g_last_actors = 0;

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

// 垂直 FOV。ini 里配一次就缓存，改了要重开面板才生效（够用）。
int FovDeg() {
    static int v = -1;
    if (v < 0) {
        v = (int)GetPrivateProfileIntW(L"esp", L"fov", 70, kIniPath);
        if (v < 20 || v > 140) v = 70;
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

    c->eye = me.pos;
    c->eye.y += sdk::kEyeHeight;

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
bool BoxOf(const Camera& cam, const sdk::Vec3& pos, float w, float h,
           float* x0, float* y0, float* x1, float* y1) {
    const float hw = sdk::kPlayerWidth * 0.5f;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < 8; ++i) {
        const float ox = (i & 1) ? hw : -hw;
        const float oy = (i & 2) ? sdk::kPlayerHeight : 0.0f;
        const float oz = (i & 4) ? hw : -hw;
        float sx = 0.0f, sy = 0.0f;
        if (!Project(cam, pos.x + ox, pos.y + oy, pos.z + oz, w, h, &sx, &sy)) {
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

}  // namespace

int LastDrawn() { return g_last_drawn; }
int LastActors() { return g_last_actors; }

void Render() {
    const int fi = FindFeature(L"ESP");
    if (fi < 0) return;
    if (!cfg::ModuleOn(fi)) {
        g_last_drawn = 0;
        return;
    }

    const uintptr_t lp = sdk::LocalPlayer();
    if (lp == 0) {
        g_last_drawn = 0;
        g_last_actors = 0;
        return;
    }

    static sdk::ActorEntry actors[512];
    const int n = sdk::EnumActors(actors, 512, lp);
    g_last_actors = n;
    if (n == 0) {
        g_last_drawn = 0;
        return;
    }

    // 找到自己（摄像机）。找不到说明 rotation 段读失败，这一帧只更新计数。
    const sdk::ActorEntry* me = nullptr;
    for (int i = 0; i < n; ++i) {
        if (actors[i].actor == lp) { me = &actors[i]; break; }
    }
    if (me == nullptr) {
        g_last_drawn = 0;
        return;
    }

    const int w = render::Width();
    const int h = render::Height();
    if (w <= 0 || h <= 0) return;

    Camera cam;
    MakeCamera(*me, w, h, &cam);

    // ── 样式参数 ──
    const float max_dist = ParamOf(fi, L"scan_radius", 64.0f);
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
        if (!BoxOf(cam, e.pos, (float)w, (float)h, &x0, &y0, &x1, &y1)) continue;

        // 完全在屏幕外就跳过（留一点余量，避免边缘闪烁）
        if (x1 < -64.0f || x0 > (float)w + 64.0f) continue;
        if (y1 < -64.0f || y0 > (float)h + 64.0f) continue;

        DrawBox(x0, y0, x1, y1, 1.6f, box);

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
    }
    g_last_drawn = drawn;

    // 限流日志：ESP 不出框的时候，这三行数字能直接定位是哪一段断了。
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last > 3000) {
        last = now;
        LogLine("[esp] actors=%d drawn=%d pos=(%.1f,%.1f,%.1f) yaw=%.1f pitch=%.1f "
                "fov=%d screen=%dx%d",
                n, drawn, me->pos.x, me->pos.y, me->pos.z, me->yaw, me->pitch,
                FovDeg(), w, h);
    }
}

}  // namespace esp
}  // namespace xiaoyu
