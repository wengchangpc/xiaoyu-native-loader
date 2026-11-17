// sdk.h - 已确认的偏移与链路（全部实测，不是猜的）
//
// 这个文件是"战果存档"：每一条都标注了怎么发现的、怎么验证的。
// 后面写模块时只认这里的东西，不再回头翻日志。
//
// ────────────────────────────────────────────────────────────────
// 目标客户端身份（★ 曾经搞错过一次，记在这里防止再错）
// ────────────────────────────────────────────────────────────────
//   真实运行路径  C:\MCLDownload\MinecraftBENetease\x64_mc\Minecraft.Windows.exe
//   不是          C:\Users\<user>\AppData\Local\Chronal\Bedrock\versions\...\
//                 那个只是磁盘上的一份，没有在跑。
//
//   构建路径（从 .exp 符号表读出来的）
//     D:/win/release/minecraftPE/build/win32_x64_netease_haldra_launcher/
//     Minecraft/Minecraft.Windows
//
//   渲染是 D3D12，不是 OpenGL：
//     目录里有 dxcompiler.dll / dxil.dll / nvngx_dlss.dll，
//     .exp 里 24 个 NVSDK_NGX_D3D12_* 导入（CreateFeature / EvaluateFeature /
//     GetParameters ...）。
//     曾经在 dllmain.cpp 里写过"实测是 OpenGL、没有 DXGI 交换链"——那个论据是
//     错的。但"不挂 Present、改用覆盖窗口"这个决定本身仍然成立，因为覆盖窗口
//     和游戏的渲染方式完全解耦。
//
//   PE 磁盘上完全加密：65,838,088 字节，ClientInstance / LocalPlayer / Actor /
//   Vec3 / BlockPos 等字符串全部 0 命中，前 7 个节连名字都没有，
//   .winlice 有 15MB 虚拟空间但磁盘占用 0。运行时才解密。
//
// ────────────────────────────────────────────────────────────────
// 运行时布局
// ────────────────────────────────────────────────────────────────
//   ImageBase(磁盘)  0x140000000
//   运行时基址       ASLR，必须用 mem::Base() 拿
//   SizeOfImage      0xD8F9000
//   节数             14
//
//   第 0 节（真 .text）：RVA 0x1000，vsize 0x088E2057（143MB）
//     磁盘熵 7.97（加密） -> 运行时熵 6.88（已解密）
//   .boot 节  RVA 0xD061000，运行时熵仍 7.958（保持加密）
//   .winlice 节 RVA 0xC125000，运行时熵 5.80（运行时才有内容）
//
// ────────────────────────────────────────────────────────────────
// ★ 静态全局：LocalPlayer 可以直接从固定 RVA 读出来
// ────────────────────────────────────────────────────────────────
//   RVA 0x0B6E9648   LocalPlayer*
//
//   读法（纳秒级，不需要扫堆）：
//       uintptr_t lp = *(uintptr_t*)(mem::Base() + 0x0B6E9648);
//
//   怎么发现的：拿本次会话确认的 LocalPlayer 绝对地址当 needle，
//   扫主模块所有非 .text 节，唯一命中就在这个 RVA。落点是 VA=0x0A666000
//   （VSize 0x013A76C0 = 20.6MB，磁盘只占 1.24MB）那一节 —— 典型的运行时
//   填充 BSS/.data。
//
//   周围结构：每 0x80 字节一组，形态是
//       +0x00  shared_ptr 的 ptr（或 _Ref_count_obj2<UControlBlock> 的 ctrl）
//       +0x08  对象指针
//       +0x20  std::mutex
//   相邻几组分别是 FlightReader / UserManager::Social / ProfilingManager /
//   Bedrock::Threading::PendingConditionals / ClientBlobCacheTrackingData /
//   DBStoragePerformanceData / PlayerRenderChunkModificationLatencyTracker /
//   LevelChunkMetaDataDebug。
//   这是 Bedrock::EnableNonOwnerReferences 的静态注册表，按注册顺序排列。
//
//   ClientInstance / MultiPlayerLevel / OverworldDimension 在这一节里零命中，
//   说明它们只被堆上的对象（Minecraft）持有，没有静态指针。
//
// ────────────────────────────────────────────────────────────────
// ★ 定位链路（每一步都由反向 RTTI 验过类名）
// ────────────────────────────────────────────────────────────────
//   LocalPlayer  = *(BASE + 0x0B6E9648)                 ← 静态全局
//   BlockSource  = *(LocalPlayer + 0x0DE8)
//   Dimension    = *(BlockSource + 0x0030)      // 实测 OverworldDimension
//   Level        = *(BlockSource + 0x0020)      // 实测 MultiPlayerLevel
//
//   另一条（从 ClientInstance 往下，需要先拿到 ClientInstance）：
//   Minecraft    = *(ClientInstance + 0x01B0)
//   wrapper      = *(Minecraft + 0x00B8)
//   Level        = *(wrapper + 0x0040)
//
//   ★ Level 的实际对象是派生类 MultiPlayerLevel，所以对象的 +0 放的是
//     MultiPlayerLevel 自己的 vtable。拿基类 Level 的 vtable 去扫堆永远是
//     0 命中 —— 这是 probe 里 Level/Dimension 找不到的原因。
//
// ────────────────────────────────────────────────────────────────
// LocalPlayer（= Actor 基类布局）字段
// ────────────────────────────────────────────────────────────────
//   +0x000  vtable
//   +0x368  float  0.6           碰撞箱宽度（常量锚点，Player 的 AABB）
//   +0x374  float  1.0
//   +0x378  float  0.4
//   +0xCE0  Vec3f  mPos          当前位置
//   +0xCEC  Vec3f  mPosPrev      上一帧位置（与 mPos 交替变化）
//   +0xDE8  BlockSource*         所在维度的方块访问器
//
//   怎么确认的：差分扫描 12 次 × 0.35s，+0xCE0 三连变化幅度
//   0.674 / 1.987 / 8.683，其余字段要么不动要么只在 ±0.1 抖动。
//   +0xCEC 与 +0xCE0 交替变化 -> mPos / mPosPrev。
//
//   ★ 这两个偏移是所有 Actor 共用的（都在 Actor 基类里），explorer.cpp
//     就是拿 "vtable 在主模块 + 这两个位置都是合法坐标 + 两者距离 < 64 米"
//     当判据来扫全堆认实体的。
//
// ────────────────────────────────────────────────────────────────
// vtable 表（RVA，用于运行时校验）
// ────────────────────────────────────────────────────────────────
//   ClientInstance   RVA 0x8B9B6F0
//   LocalPlayer      RVA 0x8C98B08
//   Actor            RVA 0x8F3AA30
//   Level            RVA 0x8FE38C0
//   Dimension        RVA 0x908EE48
//   BlockSource      RVA 0x8F3EFE8
//   Container        RVA 0x8ACE800
//
//   probe.log 里有完整的 12 个类的 TypeDescriptor / COL / vtable 三元组。
//
// ────────────────────────────────────────────────────────────────
// 成员偏移（反向 RTTI 标注，vtable[-1] -> COL -> COL+12 -> TypeDescriptor）
// ────────────────────────────────────────────────────────────────
//   ClientInstance
//     +0x188  MinecraftGame
//     +0x1A0  RayTracingOptions
//     +0x1B0  Minecraft                  ★
//     +0x1C0  LevelRenderer
//     +0x1D8  LoopbackPacketSender
//     +0x1E0  VanillaMoveInputHandler
//     +0x1F0  MinecraftKeyboardManager
//     +0x1F8  HitDetectSystem
//     +0x210  VanillaSceneFactory
//     +0x760  ActorRenderDispatcher
//     +0x778  ItemInHandRenderer
//     +0x780  ItemRenderer
//
//   MultiPlayerLevel
//     +0x0410 LoopbackPacketSender
//     +0x0530 BedrockSpawner
//     +0x0560 BlockHashPalette
//     +0x0570 BlockReducer
//     +0x0590 PortalForcer
//     +0x05D0 ContainerRegistry
//     +0x0610 LevelRandom
//     +0x0630 LegacyClientNetworkHandler
//     +0x0650 Scheduler
//     +0x06B8 LevelSoundManager
//     +0x0728 EntityRegistry
//     +0x0748 Minecraft（回指）
//     +0x0768 EntitySystemsManager
//     +0x0778 FeatureTypeFactory
//     +0x07F8 BlockActorLevelListener
//     +0x0838 ActorManager               ★
//     +0x0840 SubscriptionBody<Actor&>
//
//   BlockSource
//     +0x0008 自己
//     +0x0020 MultiPlayerLevel
//     +0x0030 OverworldDimension         ★
//     +0x0788 BlockSource（另一个实例）
//     +0x07A0 MultiPlayerLevel
//     +0x07B0 OverworldDimension
//
//   OverworldDimension
//     +0x0058 自己
//     +0x0060 _Ref_count_obj2<OverworldDimension>
//     +0x00C0 MultiPlayerLevel
//     +0x00F0 BlockSource
//     +0x0140 OverworldBrightnessRamp
//     +0x0148 CompoundTag
//     +0x0150 _Ref_count_obj2<LevelChunkMetaData>
//     +0x0158 RuntimeLightingManager
//     +0x0188 BlockEventDispatcher
//     +0x0190 TaskGroup
//     +0x0198 TaskGroup
//     +0x01B0 NetworkChunkSource
//     +0x01C0 Weather
//     +0x0328 TickingAreaList
//     +0x05D8 ChunkBuildOrderPolicy
//     +0x0788 DelayActionList
//     +0x07D0 ClientDimensionExtensions
//     注：0x2600~0x2A00 那一大片是 Molang 编译产物，指针密度极低。
//
//   ActorManager
//     +0x0020 ClientActorManagerProxy     （实体表可能在这层，未验）
//
// ────────────────────────────────────────────────────────────────
// ★ 已排除的路径（避免重复劳动）
// ────────────────────────────────────────────────────────────────
//   1. Level 的直接成员里没有回指 LocalPlayer 的字段（grep 零命中）
//   2. Level 里所有 vector 候选都不是实体表：
//        两个最大的 1.5MB / 2MB 是方块更新队列的量级，元素大小对不上
//        +0x0438 的第 0 个是 BlockActorLevelListener（监听器列表）
//   3. ActorManager 里没有 vector<Actor*>；
//      逐 qword 直接当 Actor* 验坐标只出 4 个假阳性
//      （全是 _Ref_count_resource<Pack> 里的随机浮点）
//   4. Dimension 里同理
//   5. 在 ActorManager / Dimension / Level 的内存里搜 LocalPlayer 地址，
//      零命中 —— 实体表不是"对象里一串 Actor*"这种形态
//   6. 对象 +0 只会放最派生类的 vtable，扫基类 vtable 只会命中栈上的临时值
//   7. 外部扫堆会被反作弊几秒内静默终止（4 次验证），必须进程内做
//
// ────────────────────────────────────────────────────────────────
// 反作弊行为记录
// ────────────────────────────────────────────────────────────────
//   只读主模块映像（217MB，VirtualQueryEx 逐区域读）    -> 成功，0 页失败
//   遍历堆找对象 needle（4 次，各种分块策略）           -> 几秒内进程被静默终止
//                                                        日志停在途中，连
//                                                        traceback 都来不及写
//   进程内扫堆（probe / explorer）                      -> 成功
//
//   结论：反作弊盯的是"外部进程反复访问堆"这个行为模式，
//         进程内读自己的内存不受影响。
#pragma once

#include <cstdint>

#include "memory.h"

namespace xiaoyu {
namespace sdk {

// ── 静态全局 ────────────────────────────────────────────────────
constexpr uintptr_t kLocalPlayerGlobalRva = 0x0B6E9648;

// ── LocalPlayer / Actor 字段 ────────────────────────────────────
constexpr uintptr_t kOffPos = 0x0CE0;
constexpr uintptr_t kOffPosPrev = 0x0CEC;
constexpr uintptr_t kOffBlockSource = 0x0DE8;
constexpr uintptr_t kOffHitboxWidth = 0x0368;

// ── BlockSource 字段 ────────────────────────────────────────────
constexpr uintptr_t kOffBsLevel = 0x0020;
constexpr uintptr_t kOffBsDimension = 0x0030;

// ── Level(MultiPlayerLevel) 字段 ───────────────────────────────
constexpr uintptr_t kOffActorManager = 0x0838;
constexpr uintptr_t kOffLevelMinecraft = 0x0748;

// ── ClientInstance 字段 ─────────────────────────────────────────
constexpr uintptr_t kOffCiMinecraft = 0x01B0;

// ── Minecraft 字段 ──────────────────────────────────────────────
constexpr uintptr_t kOffMcLevelWrapper = 0x00B8;

// ── wrapper 字段 ────────────────────────────────────────────────
constexpr uintptr_t kOffWrapperLevel = 0x0040;

struct Vec3 {
    float x;
    float y;
    float z;
};

// 拿当前 LocalPlayer。读不到（还在标题界面 / 全局还没写）返回 0。
inline uintptr_t LocalPlayer() {
    const uintptr_t base = mem::Base();
    if (base == 0) return 0;
    return mem::ReadPtr(base + kLocalPlayerGlobalRva);
}

inline bool LocalPlayerPos(uintptr_t lp, Vec3* out) {
    if (lp == 0 || out == nullptr) return false;
    return mem::Read(lp + kOffPos, out, sizeof(Vec3));
}

inline uintptr_t BlockSourceOf(uintptr_t lp) {
    if (lp == 0) return 0;
    return mem::ReadPtr(lp + kOffBlockSource);
}

inline uintptr_t DimensionOf(uintptr_t lp) {
    const uintptr_t bs = BlockSourceOf(lp);
    if (bs == 0) return 0;
    return mem::ReadPtr(bs + kOffBsDimension);
}

inline uintptr_t LevelOf(uintptr_t lp) {
    const uintptr_t bs = BlockSourceOf(lp);
    if (bs == 0) return 0;
    return mem::ReadPtr(bs + kOffBsLevel);
}

inline uintptr_t ActorManagerOf(uintptr_t lp) {
    const uintptr_t lv = LevelOf(lp);
    if (lv == 0) return 0;
    return mem::ReadPtr(lv + kOffActorManager);
}

// 校验一个地址是不是指定 RVA 的 vtable（用于确认链路没走歪）
inline bool VtableIs(uintptr_t obj, uintptr_t rva) {
    if (obj == 0) return false;
    const uintptr_t vt = mem::ReadPtr(obj);
    return vt == mem::Base() + rva;
}

}  // namespace sdk
}  // namespace xiaoyu
