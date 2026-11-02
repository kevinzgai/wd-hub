# WORKBUDDY 看板 · Android 壳

把 `host/wb_app.py` 内嵌的看板页面装进一个没有地址栏的窗口，装到手机上用。

## 它是什么

不是把页面搬到手机上，而是**给电脑上的服务套一个 App 外壳**。

```
手机 App (WebView) ──WiFi──► 电脑 :8000 (/ + /api/*)
                                  └─ 数据来自 wb_app.py
```

页面和 13 个 `/api/*` 接口全靠电脑那一端，所以：

- 电脑没开服务 → App 显示配置页
- 不在同一 WiFi → 连不上
- 电脑 IP 变了 → 在 App 里改地址，不用重新打包

## 用之前

1. 电脑启动服务，**必须带 `--web-host 0.0.0.0`**，否则只监听 127.0.0.1，手机进不来：

   ```bash
   py host/wb_app.py --web-host 0.0.0.0
   ```

   启动日志会打印手机该填的地址，形如 `http://192.168.0.89:8000`。

2. 防火墙放行 8000（已有 Python 入站规则的话可跳过）。
3. 手机连同一个 WiFi。
4. 装 APK，首次打开填地址。

## 构建

```bash
cd wbboard/android
./gradlew assembleDebug          # 产物 app/build/outputs/apk/debug/
```

SDK 路径写在 `android/local.properties`（`C:/Android/android-sdk`）。
图标重新生成：`py tools/make_icon.py`。

## 交互约定

| 操作 | 行为 |
|---|---|
| 首次打开 | 配置页，填 IP 或 IP:端口（省略端口按 8000 补） |
| 连不上 | 自动回到配置页并显示错误原因 |
| 长按返回键 | 任何时候回配置页改地址 |
| 返回键 | 能后退就后退，否则退出 |
| 状态栏 / 导航栏 | 透明沉浸，内容自动避开系统栏 |

## 结构

```
wbboard/
├── android/                      # 原生工程（已剥离 Capacitor 运行时）
│   └── app/src/main/
│       ├── java/.../MainActivity.java
│       └── assets/setup.html     # 地址配置页
├── tools/make_icon.py            # 生成各密度图标
└── dist/wbboard-debug.apk
```

## 已知限制

- **依赖电脑在线**。这是数据源决定的，不是壳的问题。
- 服务端无鉴权。开 `--web-host 0.0.0.0` 后，同一局域网内任何设备都能访问并可调 `/api/start`、`/api/stop`。公共 WiFi 下别开。
- debug 签名。自己装没问题；要让别人装或上架，需要换正式 keystore 并出 release 包。
