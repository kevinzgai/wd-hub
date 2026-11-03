#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

LV_FONT_DECLARE(radio_font);
LV_FONT_DECLARE(radio_font_title);
// 4360 字中文字库(SourceHanSansSC 16px 子集)。首页的姓名/签名/天气/提示、
// 收音机的中文回退都依赖它 —— radio_font 只是小子集,覆盖不了。原属 codex
// 应用目录,2026-09-12 随 AI设备 模块移除迁到 fonts/,避免被一起删掉。
// 也用于小智/摩斯电键的**动态文本**(字符不可预知)。
LV_FONT_DECLARE(buddy_font_16);
// 全机共享标题字体(20px/4bpp,SourceHanSansSC 子集)。覆盖 Launcher/设置/小智
// 的静态中文标题与提示 —— 改这些界面的中文文案后必须重新生成:
//   wsl -d Ubuntu-24.04 bash /mnt/f/WORK/AI硬件/_diag/gen_ui_font.sh
LV_FONT_DECLARE(ui_font_20);
// 首页环境仪表 12px 专用紧凑字体(网格读数/风向/体感/状态)
LV_FONT_DECLARE(home_font_12);
// Five additional 20px glyphs, with existing ui_font_20 as fallback.
LV_FONT_DECLARE(pomodoro_font_20);
// Digits and colon only: do not link the full Montserrat 48px repertoire.
LV_FONT_DECLARE(pomodoro_digits_48);
// 背单词应用:16px 中英对照释义字体(仅覆盖词库用到的 1601 个汉字,离线)。
LV_FONT_DECLARE(vocab_font_16);
// 背单词应用的启动器卡片标题(20px,仅「背单词」三字,回退 ui_font_20)。
LV_FONT_DECLARE(vocab_title_20);
// WB 看板 V2 专用：12px（ASCII + buddy 全字符集，供模型/消息/提示等动态文案）
// 与 28px（状态主字/审批问句/离线/时钟数字）。生成参数见文件头 Opts。
LV_FONT_DECLARE(wb_font_12);
LV_FONT_DECLARE(wb_font_28);

#define APP_FONT_BODY  (&radio_font)
#define APP_FONT_TITLE (&radio_font_title)

#ifdef __cplusplus
}
#endif
