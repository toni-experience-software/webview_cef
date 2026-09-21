# WebView CEF

<a href="https://pub.dev/packages/webview_cef"><img src="https://img.shields.io/pub/likes/webview_cef?logo=dart" alt="Pub.dev likes"/></a> <a href="https://pub.dev/packages/webview_cef" alt="Pub.dev popularity"><img src="https://img.shields.io/pub/popularity/webview_cef?logo=dart"/></a> <a href="https://pub.dev/packages/webview_cef"><img src="https://img.shields.io/pub/points/webview_cef?logo=dart" alt="Pub.dev points"/></a> <a href="https://pub.dev/packages/webview_cef"><img src="https://img.shields.io/pub/v/webview_cef.svg" alt="latest version"/></a> <a href="https://pub.dev/packages/webview_cef"><img src="https://img.shields.io/badge/macOS%20%7C%20Windows%20%7C%20Linux-blue?logo=flutter" alt="Platform"/></a>

[English](README.md) · **简体中文**

基于 [CEF](https://bitbucket.org/chromiumembedded/cef)（Chromium Embedded Framework）的 Flutter **桌面端** WebView。它以离屏方式渲染一个完整的 Chromium 浏览器，并将画面呈现在 Flutter 的 `Texture` 上，因此网页内容能与你的 Flutter UI 在 Windows、macOS、Linux 上原生地合成在一起。

> 基于 **CEF 149（Chromium 149）**。

---

## 特性

- 🌐 **完整 Chromium 引擎** —— 现代 Web 标准、WebGL、HTML5 视频等。
- ⚡ **GPU 零拷贝渲染**（Windows 与 macOS）—— 帧直接从 Chromium 的 GPU 输出经共享纹理交给 Flutter（Windows 用 D3D11 纹理，macOS 用 IOSurface），每帧不再做 CPU 端换色，也不再做 CPU→GPU 上传。Linux 仍走软件路径。
- 🎞️ **自适应帧率**（Windows 与 macOS）—— 由显示器的垂直消隐（vblank）驱动产帧（Windows 用 `IDXGIOutput::WaitForVBlank`，macOS 用 `CVDisplayLink`），因此 webview 跟随显示器真实刷新率（如 120/144Hz），不再被限制在 60fps；静态内容则保持空闲不产帧。
- ⌨️ **中日韩输入法实时上屏** —— 原生输入法合成管线，拼音预编辑实时显示、选词正确上屏。
- 🔌 **JavaScript 桥** —— JS 调用 Dart，Dart 执行 JS。
- 🍪 **Cookie 管理** —— 读取、设置、删除 Cookie。
- 🪟 **多实例** —— 同时运行多个独立 webview。
- 📜 **用户脚本注入** —— 在文档开始/结束时注入 JS/CSS。
- 🛠️ **DevTools**、鼠标与触控板输入、导航以及加载/标题/URL 事件。

## 平台支持

| 平台 | 最低版本 | 架构 |
| --- | --- | --- |
| Windows | Windows 10 | x64 |
| macOS | macOS 12.0 | arm64 或 x86_64（仅本机架构，非 Universal） |
| Linux | — | x64、arm64 |

## 环境要求

- Flutter **>= 3.44.0**、Dart **>= 3.6.0**（已在最新稳定版 Flutter 3.44.x 上测试）。
- 原生侧需要 C++20 工具链（CEF 149 要求）—— 较新的 MSVC / Clang / GCC。

---

## 从 ≤ 0.2.2 升级

0.5.0 是一次大版本升级（Flutter 3.44 + CEF 149），在所有平台上都有破坏性变更。若你从旧版本升级，请按以下步骤操作：

- **工具链** —— 升级到 Flutter **≥ 3.44.0** / Dart **≥ 3.6.0**（原为 2.5.0 / 2.17.1）。原生构建现在需要 **C++20**（CEF 149）；请确保你的工程没有把插件 target 强制设为更低的 C++ 标准。
- **移除的 Dart API** —— `WebviewCefPlatform`、`MethodChannelWebviewCef`、`getPlatformVersion()` 已移除（同时移除了 `plugin_platform_interface` 依赖）。它们本就不是对外 API，且无替代（`getPlatformVersion` 仅返回演示值）。请只 import `package:webview_cef/webview_cef.dart`，使用 `WebviewManager` / `WebViewController`。
- **Windows** —— `initCEFProcesses` 签名变更。请更新 `windows/runner/main.cpp`：它现在接收 `HINSTANCE` 并返回子进程退出码，且必须作为 `wWinMain` 的第一条语句立即返回（见下方 Windows 安装片段）。最低系统现为 **Windows 10**。
- **macOS** —— 使用 Flutter **3.44+** 和 SwiftPM，部署目标为 **12.0+**；运行 CEF 准备命令并添加 Runner 嵌入阶段，详见下文。仅支持本机架构。

---

## 安装

### Windows

1. 添加依赖：

   ```bash
   flutter pub add webview_cef
   ```

2. 修改 `windows/runner/main.cpp`。由于 Chromium 的多进程架构，以及需要把输入/输入法和 method channel 调用路由到 Flutter 引擎线程，需要加两处钩子：

   ```cpp
   #include "webview_cef/webview_cef_plugin_c_api.h"

   int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev,
                         _In_ wchar_t *command_line, _In_ int show_command) {
     // 启动 CEF 子进程，必须放在 wWinMain 最前面。
     int exit_code = initCEFProcesses(instance);
     if (exit_code >= 0) {
       return exit_code;
     }
     // ……原有 runner 初始化……
   ```

   在消息循环里把消息转发给 CEF（启用键盘输入，并让 CEF 能向 Flutter 引擎线程投递消息）：

   ```cpp
   ::MSG msg;
   while (::GetMessage(&msg, nullptr, 0, 0)) {
     ::TranslateMessage(&msg);
     ::DispatchMessage(&msg);
     handleWndProcForCEF(msg.hwnd, msg.message, msg.wParam, msg.lParam);
   }
   ```

   > 输入法由插件自动接管，runner 无需额外代码。

首次构建时会自动从 <https://cef-builds.spotifycdn.com> 下载官方 CEF *Standard Distribution*（约 330MB）到 `third/cef`，并从源码编译 `libcef_dll_wrapper`，因此第一次构建会明显较慢。

### macOS

要求 **Flutter 3.44+**、**macOS 12.0+**、Xcode 和 `cmake`（`brew install cmake ninja`；没有 Ninja 时使用 make）。macOS **仅支持 Swift Package Manager**。

1. 在 Flutter App 目录执行：

   ```bash
   flutter pub add webview_cef
   dart run webview_cef:setup_macos
   ```

   本仓库的 `example/` 已声明依赖，只需先运行 `flutter pub get`。准备命令从 <https://cef-builds.spotifycdn.com> 下载官方 CEF，编译 Debug 和 Release 两套 wrapper/helper，并生成 `macos/Flutter/webview_cef.xcconfig`。二进制保存在插件的 `macos/third/cef`，不提交到 Git；版本和配置未变时重复执行会跳过编译。

2. 在 `macos/Runner/Configs/Debug.xcconfig` 和 `Release.xcconfig` 中都加入（Profile 通常使用 Release）：

   ```text
   #include "../../Flutter/webview_cef.xcconfig"
   ```

   与示例一致，宿主 App 不启用 macOS App Sandbox：从 `Runner/DebugProfile.entitlements` 和 `Runner/Release.entitlements` 移除 `com.apple.security.app-sandbox`。

   生成的配置指定本机架构（arm64 或 x86_64）、macOS 12.0，并关闭构建脚本沙箱。移除 target 中冲突的设置；如需更高的部署目标，在 include 之后设置。不支持 Universal 构建。

3. 在 Xcode 的 Runner 中，现有 Flutter 嵌入阶段之后添加名为 **Embed CEF** 的 Run Script 阶段：

   ```bash
   bash "${WEBVIEW_CEF_MACOS_DIR}/scripts/embed_cef_helpers.sh"
   ```

   取消勾选 **Based on dependency analysis**。脚本会嵌入 CEF framework 和五个 helper App，并按顺序签名内部动态库及 bundle。缺失、过期或架构不符时会报错并提示重新准备。Debug 使用 Debug wrapper/helper；Profile 和 Release 使用 Release，切换配置无需重新准备。

4. 执行 `flutter run -d macos` 或 `flutter build macos`，Flutter 会生成 SwiftPM 集成。若之前全局禁用了 SwiftPM，执行 `flutter config --enable-swift-package-manager`。

将 `macos/Flutter/webview_cef.xcconfig` 加入 App 的 `.gitignore`，因为它包含本机插件路径。每台开发机/CI 机器均需运行准备命令；插件位置、本机架构或 CEF 版本变化后也需重跑。示例项目已配置 include 和构建阶段。

**从 CocoaPods 迁移：**移除旧的 `WebviewCEF.install_helper_phase` Podfile 钩子和 `Embed CEF Helpers` 阶段。所有依赖均支持 SwiftPM 时，可用 `pod deintegrate` 移除 CocoaPods 集成，再删除 Podfile/lockfile、workspace 中的 Pods 引用及 Pods xcconfig include。若其他插件仍依赖 CocoaPods，可保留它们的集成；本插件不再提供 podspec。

CEF 版本仍由 [`third/download.cmake`](third/download.cmake) 固定。两种构建模式都使用 CEF Release framework，但 wrapper 的 C++ ABI 与各自插件配置匹配。SwiftPM 链接本地准备的原生库，不依赖单独托管的二进制包。

### Linux

```bash
flutter pub add webview_cef
```

首次构建时自动下载 CEF（支持 x64 与 arm64）。请确保已安装 Flutter Linux 桌面工具链（`clang`、`cmake`、`ninja-build`、`libgtk-3-dev`、`pkg-config`）。

---

## 快速开始

```dart
import 'package:flutter/material.dart';
import 'package:webview_cef/webview_cef.dart';

class MyWebView extends StatefulWidget {
  const MyWebView({super.key});
  @override
  State<MyWebView> createState() => _MyWebViewState();
}

class _MyWebViewState extends State<MyWebView> {
  late final WebViewController _controller;

  @override
  void initState() {
    super.initState();
    _controller = WebviewManager().createWebView(
      loading: const Center(child: CircularProgressIndicator()),
    );
    _init();
  }

  Future<void> _init() async {
    await WebviewManager().initialize(); // 整个 App 调用一次
    _controller.setWebviewListener(WebviewEventsListener(
      onUrlChanged: (url) => debugPrint('url => $url'),
      onLoadEnd: (controller, url) => debugPrint('loaded => $url'),
    ));
    await _controller.initialize('https://flutter.dev');
  }

  @override
  void dispose() {
    _controller.dispose();
    WebviewManager().quit(); // 仅在整个 App 退出时调用
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return ValueListenableBuilder<bool>(
      valueListenable: _controller,
      builder: (_, ready, __) =>
          ready ? _controller.webviewWidget : _controller.loadingWidget,
    );
  }
}
```

完整示例（地址栏、Cookie、JS 桥、DevTools）见 [`example/`](example/)。

---

## 用法

### 生命周期

```dart
await WebviewManager().initialize(userAgent: 'my-app/1.0'); // 每个 App 一次
final controller = WebviewManager().createWebView(loading: const Text('…'));
await controller.initialize('https://example.com');
// …
controller.dispose();
WebviewManager().quit(); // App 关闭时
```

### 导航

```dart
controller.loadUrl('https://example.com');
controller.reload();
controller.goBack();
controller.goForward();
controller.openDevTools();
```

### 事件

```dart
controller.setWebviewListener(WebviewEventsListener(
  onTitleChanged: (title) {},
  onUrlChanged: (url) {},
  onLoadStart: (controller, url) {},
  onLoadEnd: (controller, url) {},
  onConsoleMessage: (level, message, source, line) {},
));
```

### JavaScript 桥

```dart
// Dart -> JS
controller.executeJavaScript("document.title = 'set from Dart'");
final result = await controller.evaluateJavascript("1 + 1"); // "2"

// JS -> Dart
controller.setJavaScriptChannels({
  JavascriptChannel(
    name: 'Print',
    onMessageReceived: (msg) {
      debugPrint(msg.message);
      controller.sendJavaScriptChannelCallBack(
          false, "{'code':'200'}", msg.callbackId, msg.frameId);
    },
  ),
});
```

### Cookie

```dart
await WebviewManager().setCookie('example.com', 'key', 'value');
await WebviewManager().deleteCookie('example.com', 'key');
final all = await WebviewManager().visitAllCookies();
final some = await WebviewManager().visitUrlCookies('example.com', false);
```

### 用户脚本注入

```dart
final scripts = InjectUserScripts()
  ..add(UserScript("console.log('at document start')", ScriptInjectTime.LOAD_START))
  ..add(UserScript("console.log('at document end')", ScriptInjectTime.LOAD_END));

final controller = WebviewManager().createWebView(injectUserScripts: scripts);
```

---

## Windows 构建选项

以下 CMake 选项可设置在插件目标上（括号内为默认值）：

| 选项 | 默认 | 作用 |
| --- | --- | --- |
| `WEBVIEW_CEF_GPU_TEXTURE` | `ON` | GPU 零拷贝渲染（CEF `OnAcceleratedPaint` → Flutter GPU 纹理）。设为 `OFF` 回退到软件像素缓冲路径。 |
| `WEBVIEW_CEF_USE_DEBUG_CEF` | `OFF` | 即使在 Debug 构建中也链接/打包 CEF 的 **Debug** 二进制。默认情况下 Debug 构建使用 Release 的 CEF 二进制，因为 CEF 的 Debug DCHECK 会在输入法过程中使离屏渲染崩溃。仅当你需要单步调试 CEF 自身时才打开。 |

## 升级 CEF

CEF/Chromium 版本由 [`third/download.cmake`](third/download.cmake) 的 `CEF_VERSION` 管理。Windows/Linux 自动下载；macOS 修改版本后，在 App 目录重跑 `dart run webview_cef:setup_macos`。CI 使用相同脚本和版本。

---

## 演示

<kbd>![demo](https://user-images.githubusercontent.com/7610615/190432410-c53ef1c4-33c2-461b-af29-b0ecab983579.gif)</kbd>

### 截图

| Windows | macOS | Linux |
| --- | --- | --- |
| <img src="https://user-images.githubusercontent.com/7610615/190431027-6824fac1-015d-4091-b034-dd58f79adbcb.png" width="400" /> | <img src="https://user-images.githubusercontent.com/7610615/190911381-db88cf33-70a2-4abc-9916-e563e54eb3f9.png" width="400" /> | <img src ="https://github.com/hlwhl/webview_cef/assets/49640121/50a4c2f6-1f24-4d10-9913-ad274d76cf3f" width="400" /> |
| <img src="https://user-images.githubusercontent.com/7610615/190431037-62ba0ea7-f7d1-4fca-8ce1-596a0a508f93.png" width="400" /> | <img src="https://user-images.githubusercontent.com/7610615/190911410-bd01e912-5482-4f9e-9dae-858874e5aaed.png" width="400" /> | <img src="https://github.com/hlwhl/webview_cef/assets/49640121/10a693d5-4ee0-4389-a1e8-1b0355f7c0a6" width="400" /> |

---

## 路线图

- [x] Windows / macOS / Linux 支持
- [x] 多实例
- [x] JavaScript 桥与 Cookie 管理
- [x] 输入法支持（Windows / macOS / Linux；已用中文输入法测试）
- [x] 鼠标与触控板输入
- [x] DevTools
- [x] GPU 零拷贝渲染与自适应帧率（Windows 与 macOS）
- [x] macOS SwiftPM 集成、CEF 准备命令及多进程 helper 嵌入
- [ ] macOS Universal（arm64 + x86_64）构建（需要 lipo 合并的 CEF）
- [ ] Windows 上无撕裂的 GPU 同步（keyed-mutex）

欢迎提交 PR。每个 PR 都会在 Windows、macOS、Linux 上运行构建 + `flutter analyze` CI。

## 致谢

灵感来自 [**`flutter_webview_windows`**](https://github.com/jnschulze/flutter-webview-windows)。

## 许可证

[Apache License 2.0](LICENSE)。
