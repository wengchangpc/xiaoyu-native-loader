// theme.h - 配色，数值全部来自原版 ClickGui.py 的 nilore 皮肤
#pragma once

#include "render.h"

namespace xiaoyu {
namespace theme {

// NILORE_BG (0.0902,0.0902,0.0902)  #171717，面板用 0.94 透明度
inline const D2D1_COLOR_F kBg = render::Rgba(23, 23, 23, 240);
// 栏体：原版 body 用 NILORE_BG 不透明
inline const D2D1_COLOR_F kBody = render::Rgba(23, 23, 23, 255);
// NILORE_BG_HEAD (0.0745) -> 19,19,19
inline const D2D1_COLOR_F kHeadBg = render::Rgba(19, 19, 19, 255);
// 栏头 hover 提亮到 0.1216 -> 31,31,31
inline const D2D1_COLOR_F kHeadHover = render::Rgba(31, 31, 31, 255);
// NILORE_ROW (0.1255) -> 32,32,32
inline const D2D1_COLOR_F kRow = render::Rgba(32, 32, 32, 255);
// NILORE_ROW_HOVER (0.1529) -> 39,39,39
inline const D2D1_COLOR_F kRowHover = render::Rgba(39, 39, 39, 255);
// NILORE_ACCENT -> 45,232,202  #2DE8CA
inline const D2D1_COLOR_F kAccent = render::Rgba(45, 232, 202, 255);
inline const D2D1_COLOR_F kAccentD = render::Rgba(37, 196, 171, 255);
inline const D2D1_COLOR_F kAccentDD = render::Rgba(30, 158, 139, 255);
// NILORE_OFF 白 0.62
inline const D2D1_COLOR_F kOff = render::Rgba(235, 235, 240, 158);
// NILORE_DIM 白 0.34
inline const D2D1_COLOR_F kDim = render::Rgba(217, 217, 224, 87);
// NILORE_SUB 白 0.72
inline const D2D1_COLOR_F kSub = render::Rgba(217, 217, 224, 184);
// 折叠/展开箭头 白 0.55
inline const D2D1_COLOR_F kArrow = render::Rgba(217, 217, 224, 140);
// 阴影 黑 0.30
inline const D2D1_COLOR_F kShadow = render::Rgba(0, 0, 0, 77);
// 滑轨 白 0.14
inline const D2D1_COLOR_F kTrack = render::Rgba(255, 255, 255, 36);
// 滚动条 白 0.10 / 0.40 / 0.58（最后一个是悬停/拖动时）
inline const D2D1_COLOR_F kScrollTrack = render::Rgba(255, 255, 255, 26);
inline const D2D1_COLOR_F kScrollThumb = render::Rgba(255, 255, 255, 102);
inline const D2D1_COLOR_F kScrollThumbHot = render::Rgba(255, 255, 255, 150);

}  // namespace theme
}  // namespace xiaoyu
