/* 看板取数任务
 *
 * 一个任务串起整条链路：节流上报板载指标 → GET /status 或 /screen/<i>
 * → 解析 → 写共享快照。切屏 / 改服务地址时用任务通知立刻打断等待。
 *
 * 拆出 main.cpp 的原因：这是唯一"会阻塞等网络"的地方，和 UI 的纯渲染
 * 混在一起时，排查"屏幕不刷新"要在一千多行里分辨哪部分是网络、哪部分是绘制。
 */
#pragma once

/* 启动取数任务（只调一次）。
 *
 * current_page：每轮现问"该拉第几屏"。屏号归 UI 管（按键改、休眠时可冻结），
 *               取数任务不缓存它 —— 改屏不用重启、也不用通知模块。
 * last_btn：    只用于日志（"btn=BOOT" 能看出最后一轮是哪个键触发的）。
 *               传 NULL 就打印 none。 */
void data_fetch_start(int (*current_page)(void), const char *(*last_btn)(void));

/* 叫醒取数任务立刻重拉，别干等下一个 5 秒周期。
 * 切屏、改完服务地址、退出省电时调用。任务还没起来时是空操作。 */
void data_fetch_kick(void);
