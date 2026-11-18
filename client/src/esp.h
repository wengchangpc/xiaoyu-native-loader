// esp.h - 实体透视（第一个读游戏内存的模块）
//
// 数据来源全部走 sdk.h 里已经实测过的链路：
//   LocalPlayer(静态全局) -> BlockSource -> Level -> ActorManager
//     -> registry A -> ActorOwnerComponent storage -> Actor* 数组
//     -> registry B -> RotationComponent storage -> 每个实体的 yaw/pitch
//
// 绘制走覆盖窗口自带的 Direct2D（render.h），不碰游戏的渲染管线，
// 所以不需要挂 Present / Hook D3D，也就不会因为客户端更新而失效。
//
// ★ 这个模块现在有一个「脚本可覆盖」的身份：
//   DLL 自己画的那一份只是保底，调试参数靠管道实时改（rpc 的 esp / fov 命令），
//   真正的功能逻辑搬到脚本侧（脚本用 actors + draw 命令自己投影、自己画）。
//   两边共用同一套相机数学，这样脚本调好了直接能把常数抄回这里。
#pragma once

namespace xiaoyu {
namespace esp {

// 每帧调用一次（overlay.cpp 的 RenderOnce 里，在 GUI 之前）。
// 模块没开、或者还没进世界，直接返回。
void Render();

// 上一帧实际画出来的玩家数（调试/排障用）
int LastDrawn();

// 上一帧枚举到的实体总数
int LastActors();

// ── 运行期覆盖（管道命令用，不进配置文件）──────────────────────
// 调 ESP 对齐的时候不想反复重启：直接 esp 1 / esp 0 开关。
//   -1 = 听配置文件的（默认）   0 = 强制关   1 = 强制开
void SetForce(int v);
int Force();

// 垂直 FOV 覆盖。0 = 用 C:\Xiaoyu\esp.ini 里的值。
// 真实 FOV 一旦从游戏里读出来，就让脚本调这个命令设进来。
void SetFovOverride(int deg);
int FovOverride();

// 相机原点的额外 Y 偏移。0 = 用 esp.ini 的 eye_offset（默认也是 0）。
//
// ★ 不是"眼睛高度"。实测 mPos 本身就是眼睛位置（见 sdk.h 那段反证），
//   所以正常情况下这里应该是 0。留着它是为了：
//     1. 第三人称时做微调（真实相机在身后，方向对位置差一截）；
//     2. 验证语义 —— 设成 ±1.62 画面应该整体上下平移而不是缩放。
void SetEyeOverride(float h);
float EyeOverride();

}  // namespace esp
}  // namespace xiaoyu
