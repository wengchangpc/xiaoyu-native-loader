// esp.h - 实体透视（第一个读游戏内存的模块）
//
// 数据来源全部走 sdk.h 里已经实测过的链路：
//   LocalPlayer(静态全局) -> BlockSource -> Level -> ActorManager
//     -> registry A -> ActorOwnerComponent storage -> Actor* 数组
//     -> registry B -> RotationComponent storage -> 每个实体的 yaw/pitch
//
// 绘制走覆盖窗口自带的 Direct2D（render.h），不碰游戏的渲染管线，
// 所以不需要挂 Present / Hook D3D，也就不会因为客户端更新而失效。
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

}  // namespace esp
}  // namespace xiaoyu
