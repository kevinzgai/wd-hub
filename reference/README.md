# 上游参考文件（仅保留本机型相关部分）

来源：`78/xiaozhi-esp32` 仓库 `main/boards/nologo/xingzhi-cube-1.54tft-wifi/`
许可证见同目录 `LICENSE`（MIT）。

原先整仓克隆的 `xiaozhi-esp32-st7789-main/` 是**残缺副本**（缺 `main/boards/`、
`main/display/`，只剩 application.cc 等 5 个文件），对本机型无参考价值，已删除。
下面这些文件是当时实际用到的那几个，单独保留供后续查证。

## 文件用途

| 文件 | 用途 |
|---|---|
| `config.h` | 板级引脚定义，`firmware/main/board_config.h` 据此写成 |
| `xingzhi-cube-1.54tft-wifi.cc` | 板级初始化，含 SPI 总线参数与面板配置 |
| `lcd_display.cc` | LVGL 显示层，**`swap_bytes = 1` 就在这里**，是修掉文字花屏的依据 |
| `board.jpg` | 实物板照片，接线核对用 |

## 引脚对照（已与我们的固件逐项核对一致）

| 功能 | GPIO |
|---|---|
| SCK / SDA(MOSI) | 9 / 10 |
| DC / CS / RST | 8 / 14 / 18 |
| 背光 BL | 13 |
| BOOT 按键 | 0 |

屏幕 240×240，`DISPLAY_SWAP_XY=false`，背光低电平有效（`BACKLIGHT_INVERT=false`）。

## 关键结论

上游面板配置里 `flags.swap_bytes = 1`，我们最初漏了这一步。ST7789 走 SPI 时
按高字节优先收 RGB565，而 LVGL 缓冲是小端，少这层翻转后纯黑纯白不受影响、
深色主题色差极小，唯独抗锯齿文字的中间灰度被拆错，表现为"字边彩色噪点"。

我们不用上游的 `lvgl_port`，改用 LVGL 原生方式实现同样效果：

```c
lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
```

## 关于引脚的一个提醒

`config.h` 里另有 `VOLUME_UP_BUTTON_GPIO 40` / `VOLUME_DOWN_BUTTON_GPIO 39`，
但本机这块板子没有这两个按键，固件只用了 BOOT(GPIO0)。
不要照抄上游的三键逻辑。
