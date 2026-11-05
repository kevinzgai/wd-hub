// passport-fw/main/wb/wb_theme.h —— WD Hub 方向 A「仪表像素」设计 token。
//
// 版式与取值来源：../../wd-hub-design/WD-Hub-样式规范.md 第 3 / 5 节。
// 这里**不复用**本工程 main/ui/ui_palette.h（那是 launcher 的 playful 主题，
// 色值与 travel 调色板不同）。WD Hub 是独立看板，颜色按设计稿逐位写死，
// 避免整机主题切换时把看板带偏。
//
// 所有色值都是 uint32_t 裸 hex，调用方统一用 lv_color_hex(WD_XXX) 转换。
#pragma once

// ---- 颜色（travel 调色板，规范 §3.1 / §7.1）----
#define WD_BG          0x181B1Cu   // 屏幕底色 / 待机背景
#define WD_CARD        0x202526u   // 卡片 / 页脚底板
#define WD_MUTED       0x303738u   // 页头底板 / 轨道 / 摘要条
#define WD_BORDER      0x59605Du   // 1px 描边 / 分隔线
#define WD_TEXT        0xEEE7D6u   // 主文字
#define WD_TEXT_DIM    0xB3B2A5u   // 次要文字 / ID / 离线
#define WD_PRIMARY     0xDDB36Cu   // 品牌 / 选中 / 大数字 / 数据条
#define WD_ON_PRIMARY  0x181B1Cu   // 琥珀底上的文字
#define WD_SUCCESS     0x9DB89Au   // 在线 / 运行 / 完成
#define WD_DANGER      0xF09B87u   // 高危 / 拒绝 / 等待

// ---- 几何（规范 §5.1，单位 px）----
#define WD_SCR_W       240
#define WD_SCR_H       320
#define WD_HEAD_H      32               // 页头 0..32
#define WD_CONTENT_Y   38               // 内容区 38..288
#define WD_CONTENT_H   250
#define WD_FOOT_Y      294              // 页脚 4,294,232,22
#define WD_FOOT_X      4
#define WD_FOOT_W      232
#define WD_FOOT_H      22
#define WD_CARD_X      4                // 卡片一律 x=4 w=232
#define WD_CARD_W      232
#define WD_GAP         6                // 卡片间距
#define WD_RADIUS      4                // 卡片 / 页脚圆角；其它组件 0
#define WD_LINE_H      20               // 16px 字行高
