// swift-tools-version: 5.9
import Foundation
import PackageDescription

// Flutter loads this package through a symlink. Resolve it before locating
// shared sources and the artifacts prepared by setup_macos.
let packageDirectory = URL(fileURLWithPath: #filePath)
    .resolvingSymlinksInPath().deletingLastPathComponent()
let cef = packageDirectory.appendingPathComponent("../third/cef").standardized.path

let common = packageDirectory.appendingPathComponent("../../common").standardized.path

// Preparation is explicit; manifest evaluation never downloads or builds CEF.
for artifact in ["include/cef_version.h", "Debug/libcef_dll_wrapper.a",
                 "Release/libcef_dll_wrapper.a", "Debug/cef_helper", "Release/cef_helper",
                 "Chromium Embedded Framework.framework/Chromium Embedded Framework"] {
    guard FileManager.default.fileExists(atPath: "\(cef)/\(artifact)") else {
        fatalError("Missing CEF artifact: \(artifact). Run dart run webview_cef:setup_macos from your Flutter app directory before building.")
    }
}

let package = Package(
    name: "webview_cef",
    platforms: [.macOS("12.0")],
    products: [.library(name: "webview-cef", targets: ["webview_cef"])],
    dependencies: [.package(name: "FlutterFramework", path: "../FlutterFramework")],
    targets: [
        .target(
            name: "webview_cef",
            dependencies: [.product(name: "FlutterFramework", package: "FlutterFramework")],
            cSettings: [.headerSearchPath("include/webview_cef")],
            cxxSettings: [
                .define("WEBVIEW_CEF_GPU_TEXTURE", to: "1"),
                .define("NDEBUG", .when(configuration: .release)),
                .unsafeFlags(["-I", cef, "-I", common])
            ],
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("Foundation"),
                .linkedFramework("Metal"),
                .linkedFramework("CoreVideo"),
                .linkedFramework("IOSurface"),
                .unsafeFlags(["-F", cef, "-framework", "Chromium Embedded Framework", "-ObjC"]),
                .unsafeFlags(["-L", "\(cef)/Debug", "-lcef_dll_wrapper"], .when(configuration: .debug)),
                .unsafeFlags(["-L", "\(cef)/Release", "-lcef_dll_wrapper"], .when(configuration: .release))
            ]
        )
    ],
    cxxLanguageStandard: .cxx20
)
