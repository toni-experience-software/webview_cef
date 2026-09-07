#include "webview_cef_plugin.h"
#include "webview_cef_keyevent.h"
// WebviewHandler::liveBrowserCount() — the frame pump runs only while a browser
// is actually alive (see VsyncThreadProc).
#include "webview_handler.h"
#ifdef WEBVIEW_CEF_GPU_TEXTURE
#include "webview_cef_gpu_texture.h"
#endif
// This must be included before many other Windows headers.
#include <windows.h>
#include <imm.h>
#include <commctrl.h>

// For getPlatformVersion; remove unless needed for your plugin implementation.
#include <VersionHelpers.h>

#include <flutter/method_channel.h>
#include <flutter_windows.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <algorithm>
#include <memory>
#include <thread>
#include <iostream>
#include <mutex>
#include <atomic>
#include <chrono>
#include <vector>

namespace webview_cef {
	class WebviewTextureRenderer : public WebviewTexture{
	public:
		WebviewTextureRenderer(FlutterDesktopTextureRegistrarRef texture_registrar) {
			registrar_ = texture_registrar;
			texture = std::make_unique<flutter::TextureVariant>(
				flutter::PixelBufferTexture([this](size_t width, size_t height) -> const FlutterDesktopPixelBuffer* {
					return this->CopyPixelBuffer(width, height);
				}));
			FlutterDesktopTextureInfo info = {};
			info.type = kFlutterDesktopPixelBufferTexture;
			info.pixel_buffer_config.user_data = std::get_if<flutter::PixelBufferTexture>(texture.get());
			info.pixel_buffer_config.callback = [](size_t width, size_t height, void* user_data) -> const FlutterDesktopPixelBuffer* {
				auto texture = static_cast<flutter::PixelBufferTexture*>(user_data);
				return texture->CopyPixelBuffer(width, height);
			};
			textureId = FlutterDesktopTextureRegistrarRegisterExternalTexture(registrar_, &info);
		}

		virtual ~WebviewTextureRenderer() {
			std::lock_guard<std::mutex> autolock(mutex_);
			if(registrar_){
				// FlutterDesktopTextureRegistrarUnregisterExternalTexture(registrar_, textureId, nullptr, nullptr);
			}
		}

		const FlutterDesktopPixelBuffer *CopyPixelBuffer(size_t width, size_t height) const{
			std::lock_guard<std::mutex> autolock(mutex_);
    		return pixel_buffer.get();
		}

		virtual void onFrame(const void* buffer, int width, int height) override{
			const std::lock_guard<std::mutex> autolock(mutex_);
			if (!pixel_buffer.get() || pixel_buffer.get()->width != width || pixel_buffer.get()->height != height) {
				if (!pixel_buffer.get()) {
					pixel_buffer = std::make_unique<FlutterDesktopPixelBuffer>();
					pixel_buffer->release_context = nullptr;
				}
				pixel_buffer->width = width;
				pixel_buffer->height = height;
				const auto size = width * height * 4;
				backing_pixel_buffer.reset(new uint8_t[size]);
				pixel_buffer->buffer = backing_pixel_buffer.get();
			}

			SwapBufferFromBgraToRgba((void*)pixel_buffer->buffer, buffer, width, height);
			if(registrar_){
				FlutterDesktopTextureRegistrarMarkExternalTextureFrameAvailable(registrar_, textureId);
			}
		}

		FlutterDesktopTextureRegistrarRef registrar_;
		std::unique_ptr<flutter::TextureVariant> texture;
		mutable std::shared_ptr<FlutterDesktopPixelBuffer> pixel_buffer;
		std::unique_ptr<uint8_t> backing_pixel_buffer;
		mutable std::mutex mutex_;
	};

	static flutter::EncodableValue encode_wvalue_to_flvalue(WValue* args) {
		// A null value (or a Null-typed WValue) maps to a null EncodableValue.
		// Note: EncodableValue(nullptr) must NOT be used — under C++20 it resolves
		// to std::string(const char*=nullptr) and crashes in strlen.
		if (args == nullptr) {
			return flutter::EncodableValue();
		}
		WValueType type = webview_value_get_type(args);
		switch(type){
			case Webview_Value_Type_Bool:
				return flutter::EncodableValue(webview_value_get_bool(args));
			case Webview_Value_Type_Int:
				return flutter::EncodableValue(webview_value_get_int(args));
			case Webview_Value_Type_Float:
				// flutter::EncodableValue has no scalar float alternative; C++20's
				// stricter std::variant rules no longer auto-promote float to double.
				return flutter::EncodableValue(static_cast<double>(webview_value_get_float(args)));
			case Webview_Value_Type_Double:
				return flutter::EncodableValue(webview_value_get_double(args));
			case Webview_Value_Type_String:
				return flutter::EncodableValue(webview_value_get_string(args));
			case Webview_Value_Type_Uint8_List: {
				const uint8_t* data = webview_value_get_uint8_list(args);
				return flutter::EncodableValue(std::vector<uint8_t>(data, data + webview_value_get_len(args)));
			}
			case Webview_Value_Type_Int32_List: {
				const int32_t* data = webview_value_get_int32_list(args);
				return flutter::EncodableValue(std::vector<int32_t>(data, data + webview_value_get_len(args)));
			}
			case Webview_Value_Type_Int64_List: {
				const int64_t* data = webview_value_get_int64_list(args);
				return flutter::EncodableValue(std::vector<int64_t>(data, data + webview_value_get_len(args)));
			}
			case Webview_Value_Type_Float_List: {
				const float* data = webview_value_get_float_list(args);
				return flutter::EncodableValue(std::vector<float>(data, data + webview_value_get_len(args)));
			}
			case Webview_Value_Type_Double_List: {
				const double* data = webview_value_get_double_list(args);
				return flutter::EncodableValue(std::vector<double>(data, data + webview_value_get_len(args)));
			}
			case Webview_Value_Type_List:
			{
				flutter::EncodableList ret;
				size_t len = webview_value_get_len(args);
				for (size_t i = 0; i < len; i++) {
                	ret.push_back(encode_wvalue_to_flvalue(webview_value_get_list_value(args, i)));
				}
				return ret;
			}
			case Webview_Value_Type_Map:
			{
				flutter::EncodableMap ret;
				size_t len = webview_value_get_len(args);
				for (size_t i = 0; i < len; i++) {
					ret[encode_wvalue_to_flvalue(webview_value_get_key(args, i))] = encode_wvalue_to_flvalue(webview_value_get_value(args, i));
				}
				return ret;
			}
			default:
				return flutter::EncodableValue();
		}
	}

	static WValue *encode_flvalue_to_wvalue(flutter::EncodableValue* args) {
		size_t index = args->index();
		if (index == 1) {
			return webview_value_new_bool(*std::get_if<bool>(args));
		}
		else if (index == 2 || index == 3) {
			return webview_value_new_int(*std::get_if<int32_t>(args));
		}
		else if (index == 4) {
			return webview_value_new_double(*std::get_if<double>(args));
		}
		else if (index == 5) {
			return webview_value_new_string((*std::get_if<std::string>(args)).c_str());
		}
		else if (index == 6) {
			auto list = *std::get_if<std::vector<uint8_t>>(args);
			return webview_value_new_uint8_list(list.data(), list.size());
		}
		else if (index == 7) {
			auto list = *std::get_if<std::vector<int32_t>>(args);
			return webview_value_new_int32_list(list.data(), list.size());
		}
		else if (index == 8) {
			auto list = *std::get_if<std::vector<int64_t>>(args);
			return webview_value_new_int64_list(list.data(), list.size());
		}
		else if (index == 9) {
			auto list = *std::get_if<std::vector<double>>(args);
			return webview_value_new_double_list(list.data(), list.size());
		}
		else if (index == 10) {
			WValue * ret = webview_value_new_list();
			flutter::EncodableList list = *std::get_if<flutter::EncodableList>(args);
			for (size_t i = 0; i < list.size(); i++) {
				WValue *value = encode_flvalue_to_wvalue(&list[i]);
				webview_value_append(ret, value);
				webview_value_unref(value);
			}
			return ret;
		}
		else if (index == 11) {
			WValue * ret = webview_value_new_map();
			flutter::EncodableMap map = *std::get_if<flutter::EncodableMap>(args);
			for (flutter::EncodableMap::iterator it = map.begin(); it != map.end(); it++)
			{
				WValue *key = encode_flvalue_to_wvalue(const_cast<flutter::EncodableValue *>(&it->first));
				WValue *value = encode_flvalue_to_wvalue(const_cast<flutter::EncodableValue*>(&it->second));
				webview_value_set(ret, key, value);
				webview_value_unref(key);
				webview_value_unref(value);
			}
			return ret;
		}
		else if (index == 12) {
			return nullptr;
		}
		else if (index == 13) {
			auto list = *std::get_if<std::vector<float>>(args);
			return webview_value_new_float_list(list.data(), list.size());
		}
		return nullptr;
	}

	std::unordered_map<HWND, std::shared_ptr<WebviewPlugin>> webviewPlugins;
	std::unordered_map<HWND, std::function<void(std::string method,flutter::EncodableValue * arguments)>> webviewChannels;
	// Guards webviewPlugins against the vsync driver thread's snapshot reads.
	static std::mutex g_pluginsMutex;

#ifdef WEBVIEW_CEF_GPU_TEXTURE
	// ---- Vsync-driven external BeginFrame -----------------------------------
	// With external_begin_frame_enabled the browser only produces a frame when
	// SendExternalBeginFrame is called. We tick it on every display vblank so the
	// webview's frame rate follows the monitor's actual refresh rate (adaptive,
	// not capped at 60). WaitForVBlank blocks until the next vblank and tracks
	// the current refresh rate automatically.
	static std::thread g_vsyncThread;
	static std::atomic<bool> g_vsyncRunning{false};

	static Microsoft::WRL::ComPtr<IDXGIOutput> AcquirePrimaryDxgiOutput() {
		Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
		Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
		if (FAILED(factory->EnumAdapters1(0, &adapter))) return nullptr;
		Microsoft::WRL::ComPtr<IDXGIOutput> output;
		if (FAILED(adapter->EnumOutputs(0, &output))) return nullptr;
		return output;
	}

	static UINT QueryRefreshIntervalMs() {
		DEVMODE dm = {};
		dm.dmSize = sizeof(dm);
		if (EnumDisplaySettings(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
			return (std::max)(UINT(1), UINT(1000 / dm.dmDisplayFrequency));
		}
		return 16;  // assume ~60 Hz
	}

	// With no live browser there is nothing to pace, so the pump neither waits on
	// vblank nor holds a DXGI object — it just polls this slowly. Every Flutter
	// host that *links* this plugin starts the pump at registration, including
	// hosts that pull it in transitively and never open a webview, so the idle
	// state is the common one and it has to be close to free. 200ms is ~2% of the
	// wakeups a 120Hz vblank wait costs, and is well inside the latency of
	// creating the first browser.
	static constexpr UINT kIdlePollMs = 200;

	// WaitForVBlank returns S_OK *immediately* — not an error — when the monitor
	// is asleep, the desktop is occluded, or the DXGI factory has gone stale (a
	// remote-desktop session detach does it). Without a floor on how long the
	// wait actually took, the loop becomes a 100%-of-one-core spin that floods
	// CEF with begin-frame tasks. On a display that blanks on idle that is the
	// normal state, not an edge case. Chromium guards the same call the same way
	// (ui/gl/vsync_thread_win.cc), with the same 1ms threshold.
	static constexpr auto kMinVBlankWait = std::chrono::milliseconds(1);

	static void TickAllPlugins() {
		std::vector<std::shared_ptr<WebviewPlugin>> snapshot;
		{
			std::lock_guard<std::mutex> lock(g_pluginsMutex);
			snapshot.reserve(webviewPlugins.size());
			for (auto& kv : webviewPlugins) snapshot.push_back(kv.second);
		}
		for (auto& p : snapshot) {
			if (p) p->tickBeginFrame();
		}
	}

	static void VsyncThreadProc() {
		Microsoft::WRL::ComPtr<IDXGIOutput> output;
		UINT frameMs = 16;
		UINT acquireBackoffMs = 0;
		while (g_vsyncRunning) {
			if (WebviewHandler::liveBrowserCount() == 0) {
				// Drop the output as well as the wait: holding one across a
				// display topology change is exactly what leaves the factory
				// stale, and re-acquiring on wake costs nothing at this rate.
				output.Reset();
				acquireBackoffMs = 0;
				std::this_thread::sleep_for(std::chrono::milliseconds(kIdlePollMs));
				continue;
			}

			if (!output) {
				output = AcquirePrimaryDxgiOutput();
				// Re-read the refresh rate with the output: it is the one point
				// where a mode or topology change is visible to this thread.
				frameMs = QueryRefreshIntervalMs();
				if (!output) {
					// No output to wait on: headless, a WARP/Basic-Render
					// adapter (which has none by design), or some RDP sessions.
					// Back off instead of re-enumerating DXGI at frame rate
					// forever, but keep ticking — a timer-paced frame beats no
					// frame at all.
					acquireBackoffMs = acquireBackoffMs
						? (std::min)(acquireBackoffMs * 2, 1000u)
						: frameMs;
					std::this_thread::sleep_for(std::chrono::milliseconds(acquireBackoffMs));
					TickAllPlugins();
					continue;
				}
				acquireBackoffMs = 0;
			}

			const auto waitStart = std::chrono::steady_clock::now();
			bool paced = false;
			if (SUCCEEDED(output->WaitForVBlank())) {
				paced = (std::chrono::steady_clock::now() - waitStart) >= kMinVBlankWait;
			} else {
				output.Reset();
			}
			if (!paced) {
				// The wait either failed or returned early (see kMinVBlankWait).
				// Pace off the clock so we neither spin nor stop producing.
				std::this_thread::sleep_for(std::chrono::milliseconds(frameMs));
			}
			TickAllPlugins();
		}
	}

	static void StartVsyncDriver() {
		bool expected = false;
		if (g_vsyncRunning.compare_exchange_strong(expected, true)) {
			g_vsyncThread = std::thread(VsyncThreadProc);
		}
	}

	static void StopVsyncDriver() {
		if (g_vsyncRunning.exchange(false)) {
			if (g_vsyncThread.joinable()) g_vsyncThread.join();
		}
	}
#endif  // WEBVIEW_CEF_GPU_TEXTURE

	static constexpr UINT_PTR kImeSubclassId = 1;

	// The OS IME must be intercepted in the window procedure BEFORE DefWindowProc
	// runs, otherwise the default handling consumes/converts the result string and
	// shows its own composition window. We can't do that from the runner's
	// post-DispatchMessage hook, so the Flutter view HWND is subclassed here.
	static LRESULT CALLBACK ImeSubclassProc(HWND hwnd, UINT message, WPARAM wparam,
		LPARAM lparam, UINT_PTR, DWORD_PTR) {
		switch (message) {
		case WM_IME_SETCONTEXT:
			// Don't let the OS draw its own composition window; the preedit is
			// rendered inside the web page via ImeSetComposition.
			lparam &= ~ISC_SHOWUICOMPOSITIONWINDOW;
			return DefSubclassProc(hwnd, message, wparam, lparam);
		case WM_IME_STARTCOMPOSITION:
			// Suppress default composition UI; we drive composition ourselves.
			return 0;
		case WM_IME_COMPOSITION: {
			auto pit = webviewPlugins.find(hwnd);
			if (pit == webviewPlugins.end() || !pit->second->isEditableFocused()) {
				return DefSubclassProc(hwnd, message, wparam, lparam);
			}
			HIMC imc = ImmGetContext(hwnd);
			if (imc) {
				// Committed result -> commit it to the focused browser.
				if (lparam & GCS_RESULTSTR) {
					LONG bytes = ImmGetCompositionStringW(imc, GCS_RESULTSTR, nullptr, 0);
					if (bytes > 0) {
						std::wstring ws(bytes / sizeof(wchar_t), L'\0');
						ImmGetCompositionStringW(imc, GCS_RESULTSTR, &ws[0], bytes);
						pit->second->imeCommitTextNative(ws);
					}
				}
				// Ongoing preedit -> set composition (real-time on-screen text).
				if (lparam & GCS_COMPSTR) {
					LONG bytes = ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
					if (bytes > 0) {
						std::wstring ws(bytes / sizeof(wchar_t), L'\0');
						ImmGetCompositionStringW(imc, GCS_COMPSTR, &ws[0], bytes);
						int cursor = static_cast<int>(
							ImmGetCompositionStringW(imc, GCS_CURSORPOS, nullptr, 0));
						pit->second->imeSetCompositionNative(ws, cursor);
					}
				}
				ImmReleaseContext(hwnd, imc);
			}
			// Consume: prevent DefWindowProc from showing the default IME window or
			// generating duplicate WM_IME_CHAR/WM_CHAR for the committed text.
			return 0;
		}
		case WM_IME_ENDCOMPOSITION: {
			auto pit = webviewPlugins.find(hwnd);
			// Same guard as WM_IME_COMPOSITION above, and for a second reason:
			// the subclass is installed for every host that links the plugin, so
			// without it an IME composition ending over the Flutter window
			// reaches CefPostTask before CEF exists — the pre-init fatal that
			// tickBeginFrame documents. isEditableFocused() reads only local
			// renderer state, so it is safe to call before CEF is up.
			if (pit != webviewPlugins.end() && pit->second->isEditableFocused()) {
				pit->second->imeFinishCompositionNative();
			}
			return DefSubclassProc(hwnd, message, wparam, lparam);
		}
		}
		return DefSubclassProc(hwnd, message, wparam, lparam);
	}

	void WebviewCefPlugin::RegisterWithRegistrar(FlutterDesktopPluginRegistrarRef registrar) {

		auto plugin = std::make_unique<WebviewCefPlugin>();
		plugin->m_textureRegistrar = FlutterDesktopRegistrarGetTextureRegistrar(registrar);
		flutter::PluginRegistrarWindows *window_registrar = flutter::PluginRegistrarManager::GetInstance()
																->GetRegistrar<flutter::PluginRegistrarWindows>(registrar);
		plugin->m_channel =
			std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
				window_registrar->messenger(), "webview_cef",
				&flutter::StandardMethodCodec::GetInstance());

		plugin->m_channel->SetMethodCallHandler(
			[plugin_pointer = plugin.get()](const auto &call, auto result)
			{
				plugin_pointer->HandleMethodCall(call, std::move(result));
			});

		plugin->m_hwnd = FlutterDesktopViewGetHWND(FlutterDesktopPluginRegistrarGetView(registrar));
		{
			std::lock_guard<std::mutex> lock(g_pluginsMutex);
			webviewPlugins.emplace(plugin->m_hwnd, plugin->m_plugin);
		}
#ifdef WEBVIEW_CEF_GPU_TEXTURE
		// Begin ticking external BeginFrame on every vblank (drives GPU frames).
		StartVsyncDriver();
#endif
		// Subclass the Flutter view window to intercept WM_IME_* before the engine
		// and DefWindowProc handle them (required for correct CJK composition/commit).
		SetWindowSubclass(plugin->m_hwnd, ImeSubclassProc, kImeSubclassId, 0);
		webviewChannels.emplace(plugin->m_hwnd, [plugin_pointer = plugin.get()](std::string method, flutter::EncodableValue* arguments) {
			plugin_pointer->m_channel->InvokeMethod(method, std::make_unique<flutter::EncodableValue>(*arguments));
			});
		plugin->m_plugin->setInvokeMethodFunc([plugin_pointer = plugin.get()](std::string method, WValue* arguments) {
			flutter::EncodableValue* methodValue = new flutter::EncodableValue(method);
			flutter::EncodableValue* args = new flutter::EncodableValue(encode_wvalue_to_flvalue(arguments));
			PostMessage(plugin_pointer->m_hwnd, WM_USER + 1, WPARAM(methodValue), LPARAM(args));
			});

		plugin->m_plugin->setCreateTextureFunc([plugin_pointer = plugin.get()]() {
#ifdef WEBVIEW_CEF_GPU_TEXTURE
			// Zero-copy GPU path: CEF OnAcceleratedPaint shared texture -> Flutter
			// D3D11 surface texture. Falls back to the software pixel-buffer path
			// only if the D3D11 device could not be created.
			auto gpu = std::make_shared<WebviewGpuTextureRenderer>(plugin_pointer->m_textureRegistrar);
			if (gpu->isValid()) {
				return std::dynamic_pointer_cast<WebviewTexture>(gpu);
			}
#endif
			std::shared_ptr<WebviewTextureRenderer> renderer = std::make_shared<WebviewTextureRenderer>(plugin_pointer->m_textureRegistrar);
			return std::dynamic_pointer_cast<WebviewTexture>(renderer);
		});

		window_registrar->AddPlugin(std::move(plugin));
	}
		
	WebviewCefPlugin::WebviewCefPlugin() {
		m_plugin = std::make_shared<WebviewPlugin>();
	}

	WebviewCefPlugin::~WebviewCefPlugin() {
        m_plugin = nullptr;
		RemoveWindowSubclass(m_hwnd, ImeSubclassProc, kImeSubclassId);
		bool nowEmpty = false;
		{
			std::lock_guard<std::mutex> lock(g_pluginsMutex);
			webviewPlugins.erase(m_hwnd);
			nowEmpty = webviewPlugins.empty();
		}
		webviewChannels.erase(m_hwnd);
        if(nowEmpty){
#ifdef WEBVIEW_CEF_GPU_TEXTURE
			// Stop the vsync thread before shutting CEF down. Must not hold
			// g_pluginsMutex here: the thread takes it while snapshotting.
			StopVsyncDriver();
#endif
			webview_cef::stopCEF();
		}
	}

	void WebviewCefPlugin::HandleMethodCall(
		const flutter::MethodCall<flutter::EncodableValue>& method_call,
		std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
		WValue *encodeArgs = encode_flvalue_to_wvalue(const_cast<flutter::EncodableValue *>(method_call.arguments()));
		m_plugin->HandleMethodCall(method_call.method_name(), encodeArgs, [=](int ret, WValue* args){
			if (ret > 0){
				result->Success(encode_wvalue_to_flvalue(args));
			}
			else if (ret < 0){
				result->Error("error", "error", encode_wvalue_to_flvalue(args));
			}
			else{
				result->NotImplemented();
			}
		});
		webview_value_unref(encodeArgs);
	}

	void WebviewCefPlugin::handleMessageProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
		switch (message) {
		case WM_USER + 1:
		{
			// These were heap-allocated in setInvokeMethodFunc; take ownership and
			// free them after dispatch (InvokeMethod copies the arguments).
			flutter::EncodableValue *method = (flutter::EncodableValue *)wparam;
			flutter::EncodableValue *args = (flutter::EncodableValue *)lparam;
			if (webviewPlugins.find(hwnd) != webviewPlugins.end()) {
				webviewChannels[hwnd]((*std::get_if<std::string>(method)), args);
			}
			delete method;
			delete args;
			break;
		}
		// WM_IME_* are handled in ImeSubclassProc (before DefWindowProc), not here.
		case WM_SYSCHAR:
		case WM_SYSKEYDOWN:
		case WM_SYSKEYUP:
		case WM_KEYDOWN:
		case WM_KEYUP:
		case WM_CHAR: {
			if (webviewPlugins.find(hwnd) != webviewPlugins.end()) {
				CefKeyEvent keyEvent = getCefKeyEvent(message, wparam, lparam);
				webviewPlugins[hwnd]->sendKeyEvent(keyEvent);
			}
			break;
		}
		}
	}

}  // namespace webview_cef
