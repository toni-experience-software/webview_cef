# WebView CEF

<a href="https://pub.dev/packages/webview_cef"><img src="https://img.shields.io/pub/likes/webview_cef?logo=dart" alt="Pub.dev likes"/></a> <a href="https://pub.dev/packages/webview_cef"><img src="https://img.shields.io/pub/points/webview_cef?logo=dart" alt="Pub.dev points"/></a> <a href="https://pub.dev/packages/webview_cef"><img src="https://img.shields.io/pub/v/webview_cef.svg" alt="latest version"/></a> <a href="https://pub.dev/packages/webview_cef"><img src="https://img.shields.io/badge/macOS%20%7C%20Windows%20%7C%20Linux-blue?logo=flutter" alt="Platform"/></a>

**English** · [简体中文](README.zh-CN.md)

A Flutter **desktop** WebView backed by [CEF](https://bitbucket.org/chromiumembedded/cef) (Chromium Embedded Framework). It renders a full Chromium browser off-screen and presents it inside a Flutter `Texture`, so the web content composes natively with the rest of your Flutter UI on Windows, macOS, and Linux.

> Built on **CEF 149 (Chromium 149)**.

---

## Features

- 🌐 **Full Chromium engine** — modern web standards, WebGL, HTML5 video, and more.
- ⚡ **GPU zero-copy rendering** (Windows & macOS) — frames go straight from Chromium's GPU output to Flutter via a shared texture (a D3D11 texture on Windows, an IOSurface on macOS), with no per-frame CPU color-swizzle or CPU→GPU upload. Linux still uses the software path.
- 🎞️ **Adaptive frame rate** (Windows & macOS) — frame production is driven by the display's vblank (`IDXGIOutput::WaitForVBlank` on Windows, `CVDisplayLink` on macOS), so the webview tracks your monitor's real refresh rate (e.g. 120/144 Hz) instead of being capped at 60 fps. Static content stays idle.
- ⌨️ **Real-time CJK/IME input** — native composition pipeline; Chinese/Japanese/Korean preedit appears live and commits correctly.
- 🔌 **JavaScript bridge** — call into Dart from JS and evaluate JS from Dart.
- 🍪 **Cookie management** — read, set, and delete cookies.
- 🪟 **Multiple instances** — run several independent webviews at once.
- 📜 **User-script injection** — inject JS/CSS at document start or end.
- 🛠️ **DevTools**, mouse & trackpad input, navigation, and load/title/url events.

## Supported platforms

| Platform | Minimum version | Architectures |
| --- | --- | --- |
| Windows | Windows 10 | x64 |
| macOS  | macOS 12.0 | arm64 or x86_64 (host arch only — no universal build) |
| Linux  | — | x64, arm64 |
| eLinux | — | x64, arm64 |

## Requirements

- Flutter **>= 3.44.0**, Dart **>= 3.6.0** (tested against the latest stable Flutter, 3.44.x).
- A C++20 toolchain for the native side (required by CEF 149) — recent MSVC / Clang / GCC.

---

## Upgrading from ≤ 0.2.2

0.5.0 is a large upgrade (Flutter 3.44 + CEF 149) with breaking changes on every platform. If you are coming from an older release, do the following:

- **Toolchain** — upgrade to Flutter **≥ 3.44.0** / Dart **≥ 3.6.0** (was 2.5.0 / 2.17.1). The native build now requires **C++20** (CEF 149); make sure your app doesn't force the plugin target to an older C++ standard.
- **Removed Dart API** — `WebviewCefPlatform`, `MethodChannelWebviewCef`, and `getPlatformVersion()` were removed (along with the `plugin_platform_interface` dependency). They were never the intended API and have no replacement (`getPlatformVersion` returned a demo value). Import only `package:webview_cef/webview_cef.dart` and use `WebviewManager` / `WebViewController`.
- **Windows** — `initCEFProcesses` changed signature. Update `windows/runner/main.cpp`: it now takes the `HINSTANCE` and returns a sub-process exit code that must be returned immediately, as the first statement in `wWinMain` (see the Windows install snippet below). The minimum OS is now **Windows 10**.
- **macOS** — use Flutter **3.44+** and SwiftPM, set the deployment target to **12.0+**, run CEF preparation, and add the Runner embedding phase described below. Builds remain host-architecture only.

---

## Installation

### Windows

1. Add the dependency:

   ```bash
   flutter pub add webview_cef
   ```

2. Edit `windows/runner/main.cpp`. Because of Chromium's multi-process architecture and to route input/IME and method-channel calls onto the Flutter engine thread, two hooks are required:

   ```cpp
   #include "webview_cef/webview_cef_plugin_c_api.h"

   int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev,
                         _In_ wchar_t *command_line, _In_ int show_command) {
     // Start the CEF sub-processes. MUST be the first thing in wWinMain.
     int exit_code = initCEFProcesses(instance);
     if (exit_code >= 0) {
       return exit_code;
     }
     // ... existing runner setup ...
   ```

   In the message loop, forward messages to CEF (enables keyboard input and lets CEF post to the Flutter engine thread):

   ```cpp
   ::MSG msg;
   while (::GetMessage(&msg, nullptr, 0, 0)) {
     ::TranslateMessage(&msg);
     ::DispatchMessage(&msg);
     handleWndProcForCEF(msg.hwnd, msg.message, msg.wParam, msg.lParam);
   }
   ```

   > IME is wired up automatically by the plugin — no extra runner code needed.

On the first build, the official CEF *Standard Distribution* (~330 MB, from <https://cef-builds.spotifycdn.com>) is downloaded into `third/cef` and `libcef_dll_wrapper` is compiled from source, so the first build takes noticeably longer.

### macOS

Requires **Flutter 3.44+**, **macOS 12.0+**, Xcode, and `cmake` (`brew install cmake ninja`; `make` is used if Ninja is unavailable). macOS uses **Swift Package Manager only**.

1. From your Flutter app directory, add the dependency and prepare CEF:

   ```bash
   flutter pub add webview_cef
   dart run webview_cef:setup_macos
   ```

   In this repository's `example/`, run `flutter pub get` before the setup command instead. Preparation downloads the official CEF Standard Distribution from <https://cef-builds.spotifycdn.com>, builds both Debug and Release wrappers/helpers, and writes `macos/Flutter/webview_cef.xcconfig`. Artifacts stay in the plugin's ignored `macos/third/cef` directory. Repeating the command is a no-op when the pinned version and both configurations are present.

2. Include the generated settings in **both** `macos/Runner/Configs/Debug.xcconfig` and `Release.xcconfig` (Profile normally uses Release):

   ```text
   #include "../../Flutter/webview_cef.xcconfig"
   ```

   Like the example, the host app must run without the macOS App Sandbox: remove `com.apple.security.app-sandbox` from `Runner/DebugProfile.entitlements` and `Runner/Release.entitlements`.

   These settings select the host architecture (`arm64` or `x86_64`), macOS 12.0, and disable script sandboxing for the embedding phase. Remove any conflicting target-level overrides. If your app needs a newer macOS deployment target, set it after this include. Universal builds are not supported.

3. In Xcode, add a Runner **Run Script** phase named **Embed CEF**, after the existing Flutter embedding phase:

   ```bash
   bash "${WEBVIEW_CEF_MACOS_DIR}/scripts/embed_cef_helpers.sh"
   ```

   Uncheck **Based on dependency analysis** so it runs for every build. This copies the CEF framework and all five helper apps, signs nested libraries and bundles, and fails with a setup instruction if artifacts are missing, stale, or for another architecture. Debug uses the Debug wrapper/helper; Profile and Release use Release. No preparation is needed when switching configuration.

4. Build with `flutter run -d macos` or `flutter build macos`. Flutter generates the SwiftPM integration automatically. If SwiftPM was disabled globally, enable it with `flutter config --enable-swift-package-manager`.

Do not commit `macos/Flutter/webview_cef.xcconfig`: it contains a machine-local plugin path. Add it to your app's `.gitignore`. Run setup on each development/CI machine and again after changing the plugin location, host architecture, or pinned CEF version. The example already includes the Runner settings and embedding phase.

**Migrating from CocoaPods:** remove the old `WebviewCEF.install_helper_phase` Podfile hook and `Embed CEF Helpers` build phase. For an app whose dependencies all support SwiftPM, remove CocoaPods integration (`pod deintegrate`), its Podfile/lockfile, Pods workspace references, and Pods xcconfig includes. Keep CocoaPods integration if unrelated plugins still require it; this plugin itself no longer provides a podspec. Replace the old setup with the steps above.

CEF's version remains pinned in [`third/download.cmake`](third/download.cmake). Preparation uses CEF's Release framework for both modes while matching each wrapper's C++ ABI to the plugin configuration. SwiftPM uses local native search/linker settings; there is no separately hosted binary package.

### Linux

```bash
flutter pub add webview_cef
```

CEF is downloaded automatically on the first build (x64 and arm64 supported). Make sure the usual Flutter Linux desktop toolchain is installed (`clang`, `cmake`, `ninja-build`, `libgtk-3-dev`, `pkg-config`).

---

## Quick start

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
    await WebviewManager().initialize(); // call once for the whole app
    _controller.setWebviewListener(WebviewEventsListener(
      onUrlChanged: (url) => debugPrint('url => $url'),
      onLoadEnd: (controller, url) => debugPrint('loaded => $url'),
    ));
    await _controller.initialize('https://flutter.dev');
  }

  @override
  void dispose() {
    _controller.dispose();
    WebviewManager().quit(); // only when tearing down the whole app
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

A full-featured example (navigation bar, cookies, JS bridge, DevTools) lives in [`example/`](example/).

---

## Usage

### Lifecycle

```dart
await WebviewManager().initialize(userAgent: 'my-app/1.0'); // once per app
final controller = WebviewManager().createWebView(loading: const Text('…'));
await controller.initialize('https://example.com');
// …
controller.dispose();
WebviewManager().quit(); // on app shutdown
```

### Navigation

```dart
controller.loadUrl('https://example.com');
controller.reload();
controller.goBack();
controller.goForward();
controller.openDevTools();
```

### Events

```dart
controller.setWebviewListener(WebviewEventsListener(
  onTitleChanged: (title) {},
  onUrlChanged: (url) {},
  onLoadStart: (controller, url) {},
  onLoadEnd: (controller, url) {},
  onConsoleMessage: (level, message, source, line) {},
));
```

### JavaScript bridge

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

### Cookies

```dart
await WebviewManager().setCookie('example.com', 'key', 'value');
await WebviewManager().deleteCookie('example.com', 'key');
final all = await WebviewManager().visitAllCookies();
final some = await WebviewManager().visitUrlCookies('example.com', false);
```

### User-script injection

```dart
final scripts = InjectUserScripts()
  ..add(UserScript("console.log('at document start')", ScriptInjectTime.LOAD_START))
  ..add(UserScript("console.log('at document end')", ScriptInjectTime.LOAD_END));

final controller = WebviewManager().createWebView(injectUserScripts: scripts);
```

### eLinux 🐧

For eLinux, this plugin supports **Wayland** and **DRM-GBM** backends using a decoupled architecture that avoids GTK/X11 dependencies.

#### Runtime Dependencies

Ensure the target eLinux system has the following libraries installed:

- `libnss3`
- `libnspr4`
- `libfontconfig1`
- `libasound2`

#### CEF Binary Compatibility

- **Sandbox**: CEF's sandbox is disabled by default (`--no-sandbox`) to avoid SUID permission issues common on embedded filesystems.
- **Architecture**: While this project supports x64, ensure you have the correct CEF binaries for your target architecture (ARM64 support requires corresponding CEF builds).

#### Setup

1. Ensure your eLinux toolchain is correctly configured.
2. Use the `flutter-elinux` SDK to build your application.
3. The plugin will automatically use the `elinux/` port which implements an efficient pixel buffer rendering pipeline.

#### TO RUN

`cd example/`
`flutter-elinux pub get`
`flutter-elinux build elinux --release`

`./build/elinux/x64/release/bundle/webview_cef_example -b .`

---

## Windows build options

These CMake options can be set on the plugin target (defaults shown):

| Option | Default | Effect |
| --- | --- | --- |
| `WEBVIEW_CEF_GPU_TEXTURE` | `ON` | Zero-copy GPU rendering (CEF `OnAcceleratedPaint` → Flutter GPU surface texture). Set `OFF` to fall back to the software pixel-buffer path. |
| `WEBVIEW_CEF_USE_DEBUG_CEF` | `OFF` | Link/bundle the CEF **Debug** binaries even in Debug builds. By default Debug builds use the Release CEF binaries, because CEF's Debug DCHECKs crash off-screen rendering during IME. Turn `ON` only to step into CEF itself. |

## Updating CEF

The CEF/Chromium version is pinned in [`third/download.cmake`](third/download.cmake). Windows and Linux download it automatically; on macOS, rerun `dart run webview_cef:setup_macos` from your app directory after changing the pin. CI uses the same preparation script and version.

---

## Demo

<kbd>![demo](https://user-images.githubusercontent.com/7610615/190432410-c53ef1c4-33c2-461b-af29-b0ecab983579.gif)</kbd>

### Screenshots

| Windows | macOS | Linux |
| --- | --- | --- |
| <img src="https://user-images.githubusercontent.com/7610615/190431027-6824fac1-015d-4091-b034-dd58f79adbcb.png" width="400" /> | <img src="https://user-images.githubusercontent.com/7610615/190911381-db88cf33-70a2-4abc-9916-e563e54eb3f9.png" width="400" /> | <img src ="https://github.com/hlwhl/webview_cef/assets/49640121/50a4c2f6-1f24-4d10-9913-ad274d76cf3f" width="400" /> |
| <img src="https://user-images.githubusercontent.com/7610615/190431037-62ba0ea7-f7d1-4fca-8ce1-596a0a508f93.png" width="400" /> | <img src="https://user-images.githubusercontent.com/7610615/190911410-bd01e912-5482-4f9e-9dae-858874e5aaed.png" width="400" /> | <img src="https://github.com/hlwhl/webview_cef/assets/49640121/10a693d5-4ee0-4389-a1e8-1b0355f7c0a6" width="400" /> |

---

## Roadmap

- [x] Windows / macOS / Linux support
- [x] Multiple instances
- [x] JavaScript bridge & cookie management
- [x] IME support (Windows / macOS / Linux; tested with Chinese IMEs)
- [x] Mouse & trackpad input
- [x] DevTools
- [x] GPU zero-copy rendering & adaptive frame rate (Windows & macOS)
- [x] macOS SwiftPM integration with CEF preparation and multi-process helper embedding
- [ ] Universal (arm64 + x86_64) macOS builds (needs a lipo'd CEF)
- [ ] Tear-free GPU sync (keyed-mutex) on Windows

Pull requests are welcome. Every PR runs build + `flutter analyze` CI on Windows, macOS, and Linux.

## Credits

Inspired by [**`flutter_webview_windows`**](https://github.com/jnschulze/flutter-webview-windows).

## License

[Apache License 2.0](LICENSE).

## Star History

<a href="https://www.star-history.com/?repos=hlwhl%2Fwebview_cef&type=date&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=hlwhl/webview_cef&type=date&theme=dark&legend=top-left&sealed_token=36cUYlIAsJhOcNnN3neJnDQAcumIMjtFYau6ocPNnkLDMWmw7ePDGypaleDftCKR60NGvFRODL7lcmCy7RMsbcGnccCA7bq8oHCRwnsYbe9IFj8vVu_hl9sIR_Rrs-eergCjkREudRIfEPs0attqrIRMaC--c-ovTxfnAabP9dPc2yGUz3hrhtJqSWqF" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=hlwhl/webview_cef&type=date&legend=top-left&sealed_token=36cUYlIAsJhOcNnN3neJnDQAcumIMjtFYau6ocPNnkLDMWmw7ePDGypaleDftCKR60NGvFRODL7lcmCy7RMsbcGnccCA7bq8oHCRwnsYbe9IFj8vVu_hl9sIR_Rrs-eergCjkREudRIfEPs0attqrIRMaC--c-ovTxfnAabP9dPc2yGUz3hrhtJqSWqF" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=hlwhl/webview_cef&type=date&legend=top-left&sealed_token=36cUYlIAsJhOcNnN3neJnDQAcumIMjtFYau6ocPNnkLDMWmw7ePDGypaleDftCKR60NGvFRODL7lcmCy7RMsbcGnccCA7bq8oHCRwnsYbe9IFj8vVu_hl9sIR_Rrs-eergCjkREudRIfEPs0attqrIRMaC--c-ovTxfnAabP9dPc2yGUz3hrhtJqSWqF" />
 </picture>
</a>
