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
//     +0x0048 / +0x0050  registry B       ★ 有 rotation / statevector
//     +0x01C8 / +0x01D0  registry
//     +0x0270 / +0x0278  registry A       ★ 有 ActorOwnerComponent
//
// ────────────────────────────────────────────────────────────────
// ★★ 实体列表：Bedrock 用的是 EnTT 风格 ECS，不是 vector<Actor*>
// ────────────────────────────────────────────────────────────────
//   这条推翻了之前所有「在 Level / Dimension / ActorManager 里找
//   vector<Actor*>」的思路。实测（recon/ecs.py + players.py + rot.py）：
//
//     Level +0x0838 -> ActorManager
//     ActorManager+0x0270 -> registry A
//     registry A +0x0558  -> basic_storage<VActorOwnerComponent, EntityId>
//     storage +0x0020     -> packed EntityId 数组（4 字节一个）★长度在这
//     storage +0x0050     -> 「instance 分段」vector，只 1 个元素
//       *(storage+0x0050) -> 裸 Actor* 数组
//
//   ★ 数组后面紧跟字符串常量（在 [33] 读到 ASCII "position"），
//     所以**不能**靠「读到非法指针就停」来取长度。
//     真实长度 = (*(storage+0x28) - *(storage+0x20)) / 4，实测 33。
//
//   ★ ActorManager 下有 6 个 registry，只有 +0x0270 那个身上挂着
//     ActorOwnerComponent；+0x01C8 的 +0x478 处是空的（0xFFFFFFFF），
//     早期按 ecs.log 里另一个 registry 的偏移去读才读得到，别搞混。
//
// ────────────────────────────────────────────────────────────────
// ★★★ 旋转（yaw / pitch）：另一个 registry，用 EntityId 索引对齐
// ────────────────────────────────────────────────────────────────
//   registry A 里只有 mob 标记 + 摄像机组件，**没有 rotation**。
//   rotation 在 registry B（ActorManager+0x0048）：
//     registry B +0x0528 -> basic_storage<UActorRotationComponent>
//     registry B +0x0548 -> basic_storage<UStateVectorComponent>
//
//   实测三个 storage（ActorOwner / Rotation / StateVector）的 packed
//   EntityId 列表**完全一致、顺序也一致**（33 个），所以下标可以直接
//   互通：actors[i] 的 rotation 就是 rot_seg[i * 0x10]。
//
//   元素布局（实测反推）：
//     UActorRotationComponent  步长 0x10
//       +0x00 float  pitch（度，低头为正）
//       +0x04 float  yaw  （度，0=+Z，90=-X）
//       +0x08 Vec2   mRotPrev（同一对值）
//     UStateVectorComponent    步长 0x30
//       +0x00 Vec3 mPos
//       +0x0C Vec3 mPosPrev
//       +0x18 Vec3 mVelocity
//   怎么确认步长的：拿 7 个 Actor 已知的 +0xCE0 坐标在 statevector 段里
//   定位，命中位置是 0x00 / 0x30 / 0x60 / 0x90 / 0xC0 / 0xF0 / 0x120，
//   等差 0x30；而且第 k 组正好是 Actor#k，跨 storage 下标对齐成立。
//   rotation 段的原始 float 是 (pitch, yaw) 成对重复两次，例如
//   玩家 6.570 / 91.916，同场景 18 个 RemotePlayer 的 yaw 集中在
//   81~92 度、pitch 都是 0 左右 —— 符合「站成一排看着同一方向」。
//
// ────────────────────────────────────────────────────────────────
// ★ ItemActor 是未激活池，别当实体用
// ────────────────────────────────────────────────────────────────
//   实体表里有 14 个 ItemActor，但它们的 +0xCE0 全是 (0,0,0)，
//   而且在对象头 0x2000 字节里找不到任何「附近坐标」的三连浮点
//   （recon/entity2.py 全扫过）。更能说明问题的是 BlockSource：
//   18 个 RemotePlayer 的 +0xDE8 全指向有效的 BlockSource，
//   14 个 ItemActor 里 13 个是 0、剩下 1 个是 0x801 这种垃圾值。
//   结论：它们还没被放进世界，ESP 只画 Player 子类。
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

// ── Minecraft 字段 ──────────────────────────────────────────────
constexpr uintptr_t kOffMcClientInstanceA = 0x0018;   // 与下面同一个对象
constexpr uintptr_t kOffMcClientInstanceB = 0x0168;

// ── ClientInstance 字段 ─────────────────────────────────────────
constexpr uintptr_t kOffCiLevelRenderer = 0x0138;
constexpr uintptr_t kOffCiOptions = 0x0CC0;

struct Vec3 {
    float x;
    float y;
    float z;
};

// 拿当前 LocalPlayer。读不到（还在标题界面 / 全局还没写）返回 0。
//
// ★ 这里必须兜底调一次 mem::Init()。以前只有 probe / explorer 会调它，
//   两个都关掉之后 mem::Base() 一直是 0，整个 SDK 静默失效 —— ESP 一进
//   渲染函数就 return，连一行日志都不会留。mem::Init() 是幂等的。
inline uintptr_t LocalPlayer() {
    uintptr_t base = mem::Base();
    if (base == 0) {
        (void)mem::Init();
        base = mem::Base();
    }
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

// Level -> Minecraft -> ClientInstance
inline uintptr_t MinecraftOf(uintptr_t lp) {
    const uintptr_t lv = LevelOf(lp);
    if (lv == 0) return 0;
    return mem::ReadPtr(lv + kOffLevelMinecraft);
}

inline uintptr_t ClientInstanceOf(uintptr_t lp) {
    const uintptr_t mc = MinecraftOf(lp);
    if (mc == 0) return 0;
    uintptr_t ci = mem::ReadPtr(mc + kOffMcClientInstanceA);
    if (ci == 0) ci = mem::ReadPtr(mc + kOffMcClientInstanceB);
    return ci;
}

// 游戏设置对象。FOV / 视角 / 灵敏度都在里面 —— 目前还没定位到具体字段，
// 先用 esp.cpp 里配置的 fov，等 Options 的字段挖出来再切过来。
inline uintptr_t OptionsOf(uintptr_t lp) {
    const uintptr_t ci = ClientInstanceOf(lp);
    if (ci == 0) return 0;
    return mem::ReadPtr(ci + kOffCiOptions);
}

// 校验一个地址是不是指定 RVA 的 vtable（用于确认链路没走歪）
inline bool VtableIs(uintptr_t obj, uintptr_t rva) {
    if (obj == 0) return false;
    const uintptr_t vt = mem::ReadPtr(obj);
    return vt == mem::Base() + rva;
}

// ══════════════════════════════════════════════════════════════════
// 高层对象链（2026-11-18 recon/dump_all.py 实测）
// ══════════════════════════════════════════════════════════════════
//   Level     +0x0748  Minecraft
//   Minecraft +0x0018  ClientInstance      ← 和 +0x0168 指向同一对象
//   Minecraft +0x0168  ClientInstance
//   Minecraft +0x0198  MinecraftGame
//
//   ClientInstance 内部（扫前 0x1400 字节）：
//     +0x00F8  MinecraftEventing
//     +0x0100  MinecraftGame
//     +0x0118  RayTracingOptions
//     +0x0128  Minecraft
//     +0x0138  LevelRenderer            ★ 世界矩阵 / 相机大概率在这层
//     +0x0168  MinecraftKeyboardManager
//     +0x0170  HitDetectSystem
//     +0x0CC0  Options                  ★ FOV / 视角 / 各设置项
//     +0x0CC8  _Ref_count_resource<$Options>
//
//   用途：ESP 现在是自算投影（玩家眼睛 + rotation 组件 + 配置里的 FOV）。
//   拿到 Options 之后 FOV 可以改成读游戏设置，投影就和真实画面完全一致；
//   LevelRenderer 里若能挖出 view-projection 矩阵，连相机都省了。
//
// ── ECS 组件清单（两次 dump 的并集，只列对模块有用的）────────────
//   UStateVectorComponent         pos / posPrev / velocity       步长 0x30
//   UActorRotationComponent       pitch / yaw                    步长 0x10
//   UAABBShapeComponent           碰撞箱（Reach / ESP 精确框）
//   UActorUniqueIDComponent       实体唯一 id（追踪、去重）
//   UOnGroundFlagComponent        是否着地（Fly / NoFall）
//   UGroundTravelFlagComponent    地面移动标志
//   UAirSpeedComponent            空中速度（Fly / Speed）
//   UFallDistanceComponent        下落距离（NoFall）
//   UHorizontalCollisionComponent 水平碰撞（Scaffold / 卡墙判断）
//   UActiveCameraComponent        当前激活相机实体（第三人称也对得上）
//   UPassengerComponent / UVehicleComponent   骑乘关系
//
//   ★ 没有 HealthComponent，也没有 InventoryComponent：血量、饥饿、背包
//     不在 ECS 里，得从 Actor / Player 的字段读。dump_all.py 第 8 节在
//     LocalPlayer 前 0x1000 字节里既没找到 float 20.0 也没找到 int 20，
//     下一步要往 0x1000 之后继续找。
//
// ══════════════════════════════════════════════════════════════════
// ECS 实体表（全部实测于 2026-11-18，33 个实体：1 本机 + 18 远程 + 14 掉落物）
// ══════════════════════════════════════════════════════════════════
// registry A（ActorOwnerComponent）在 ActorManager 上的偏移
constexpr uintptr_t kOffActorOwnerRegistry = 0x0270;
constexpr uintptr_t kOffActorOwnerStorage = 0x0558;

// registry B（rotation / statevector）在 ActorManager 上的偏移
constexpr uintptr_t kOffRotRegistry = 0x0048;
constexpr uintptr_t kOffRotStorage = 0x0528;
constexpr uintptr_t kOffStateVecStorage = 0x0548;

// basic_sparse_set / basic_storage 内部
constexpr uintptr_t kOffStoragePackedBegin = 0x0020;   // 4 字节 EntityId 数组
constexpr uintptr_t kOffStoragePackedEnd = 0x0028;     // 长度 =(end-begin)/4
constexpr uintptr_t kOffStorageSegVec = 0x0050;        // 元素（裸数组）指针

// 组件步长
constexpr size_t kRotStride = 0x10;   // { Vec2 mRot; Vec2 mRotPrev; }
constexpr size_t kStateVecStride = 0x30;  // { Vec3 pos; Vec3 prev; Vec3 vel; }

// 玩家眼睛相对脚底的高度（站立 1.62，潜行 1.27）。ESP 的投影原点用它。
constexpr float kEyeHeight = 1.62f;
// 玩家碰撞箱（玩家固定 0.6 x 1.8）
constexpr float kPlayerWidth = 0.6f;
constexpr float kPlayerHeight = 1.8f;

struct ActorEntry {
    uintptr_t actor;
    uint32_t entity_id;
    Vec3 pos;      // 脚底位置（= +0xCE0）
    float pitch;   // 度，低头为正
    float yaw;     // 度，0=+Z，90=-X
};

// ── 类型名反查：vtable -> COL -> TypeDescriptor -> 名字 ──────────
// 只在同一个 vtable 上做一次，之后走 64 项小缓存。
// 返回的指针指向静态存储，调用方不要释放。
inline const char* ClassNameOf(uintptr_t obj) {
    struct Slot {
        uintptr_t vt;
        char name[80];
    };
    static Slot s_cache[64];
    static int s_used = 0;

    const uintptr_t vt = mem::ReadPtr(obj);
    if (vt == 0) return nullptr;
    for (int i = 0; i < s_used; ++i) {
        if (s_cache[i].vt == vt) return s_cache[i].name;
    }
    if (s_used >= 64) s_used = 0;  // 满了就从 0 覆盖，几十个类足够用

    const uintptr_t base = mem::Base();
    if (!mem::InImage(vt)) return nullptr;
    const uintptr_t col = mem::ReadPtr(vt - 8);
    if (col == 0 || !mem::InImage(col)) return nullptr;
    uint32_t td_rva = 0;
    if (!mem::Read(col + 12, &td_rva) || td_rva == 0) return nullptr;

    char raw[160] = {0};
    if (!mem::Read(base + td_rva + 16, raw, sizeof(raw) - 1)) return nullptr;

    // 去掉 MSVC 修饰前缀 ".?AV" 和结尾的 "@@"
    const char* p = raw;
    if (p[0] == '.' && p[1] == '?' && p[2] == 'A') p += 3;
    if (p[0] == 'V' || p[0] == 'U') ++p;

    Slot& s = s_cache[s_used++];
    s.vt = vt;
    int n = 0;
    for (; p[n] != '\0' && p[n] != '@' && n < (int)sizeof(s.name) - 1; ++n) {
        s.name[n] = p[n];
    }
    s.name[n] = '\0';
    return s.name;
}

// 类名里带 Player 的都算玩家（LocalPlayer / RemotePlayer / 可能还有
// ServerPlayer / Player 派生类）。ItemActor、Mob 之类一律不算。
inline bool IsPlayerClass(const char* name) {
    if (name == nullptr) return false;
    for (const char* p = name; *p != '\0'; ++p) {
        if (p[0] == 'P' && p[1] == 'l' && p[2] == 'a' && p[3] == 'y' &&
            p[4] == 'e' && p[5] == 'r') {
            return true;
        }
    }
    return false;
}

// 枚举当前维度里的全部实体。返回实际写入 out 的条数。
// lp 传 0 时内部自己取。max 建议 >= 512。
inline int EnumActors(ActorEntry* out, int max, uintptr_t lp = 0) {
    if (out == nullptr || max <= 0) return 0;
    if (lp == 0) lp = LocalPlayer();
    if (lp == 0) return 0;

    const uintptr_t am = ActorManagerOf(lp);
    if (am == 0) return 0;
    const uintptr_t reg_a = mem::ReadPtr(am + kOffActorOwnerRegistry);
    if (reg_a == 0) return 0;
    const uintptr_t sto = mem::ReadPtr(reg_a + kOffActorOwnerStorage);
    if (sto == 0) return 0;

    const uintptr_t pb = mem::ReadPtr(sto + kOffStoragePackedBegin);
    const uintptr_t pe = mem::ReadPtr(sto + kOffStoragePackedEnd);
    if (pb == 0 || pe <= pb) return 0;
    int n = (int)((pe - pb) / 4);
    // 先做合理性检查再截断：正常世界几十到几千个实体，读到上百万说明
    // 链路歪了（比如 storage 被释放后在原地被别的对象复用）。
    if (n <= 0 || n > 65536) return 0;
    if (n > max) n = max;

    const uintptr_t segvec = mem::ReadPtr(sto + kOffStorageSegVec);
    if (segvec == 0) return 0;
    const uintptr_t seg = mem::ReadPtr(segvec);
    if (seg == 0) return 0;

    // rotation 段：拿不到就只填位置，pitch/yaw 留 0
    uintptr_t rot_seg = 0;
    const uintptr_t reg_b = mem::ReadPtr(am + kOffRotRegistry);
    if (reg_b != 0) {
        const uintptr_t rsto = mem::ReadPtr(reg_b + kOffRotStorage);
        if (rsto != 0) {
            const uintptr_t rv = mem::ReadPtr(rsto + kOffStorageSegVec);
            if (rv != 0) rot_seg = mem::ReadPtr(rv);
        }
    }

    int got = 0;
    for (int i = 0; i < n; ++i) {
        const uintptr_t a = mem::ReadPtr(seg + (size_t)i * 8);
        if (a == 0) continue;
        ActorEntry e;
        e.actor = a;
        e.entity_id = 0;
        e.pitch = 0.0f;
        e.yaw = 0.0f;
        (void)mem::Read(pb + (size_t)i * 4, &e.entity_id);
        if (!mem::Read(a + kOffPos, &e.pos)) continue;
        if (rot_seg != 0) {
            float rp[2] = {0.0f, 0.0f};
            if (mem::Read(rot_seg + (size_t)i * kRotStride, rp, sizeof(rp))) {
                e.pitch = rp[0];
                e.yaw = rp[1];
            }
        }
        out[got++] = e;
    }
    return got;
}

// 只取玩家（含本机）。返回条数。
inline int EnumPlayers(ActorEntry* out, int max, uintptr_t lp = 0) {
    if (out == nullptr || max <= 0) return 0;
    ActorEntry tmp[512];
    int n = EnumActors(tmp, 512, lp);
    int got = 0;
    for (int i = 0; i < n && got < max; ++i) {
        if (!IsPlayerClass(ClassNameOf(tmp[i].actor))) continue;
        out[got++] = tmp[i];
    }
    return got;
}

}  // namespace sdk
}  // namespace xiaoyu
