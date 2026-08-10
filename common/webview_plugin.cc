#include "webview_plugin.h"

#ifdef OS_MAC
#include <include/wrapper/cef_library_loader.h>
#endif

#include <math.h>
#include <chrono>
#include <memory>
#include <thread>
#include <iostream>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>
#endif

namespace webview_cef {
	CefMainArgs mainArgs;
	CefRefPtr<WebviewApp> app;
	CefString userAgent;
	bool isCefInitialized = false;
	// Whether CefInitialize actually succeeded this session — CefShutdown
	// without a successful init crashes, so stopCEF() gates on it.
	static bool g_cefInitOk = false;
#ifdef _WIN32
	// The persistent profile dir (empty when running cache-less). Used by the
	// clean-exit marker below.
	static std::wstring g_cacheDir;
	// Present in the profile dir iff the previous run shut down cleanly. Removed
	// at startup (this run is now "dirty"), written after CefShutdown completes.
	// A missing marker on a non-empty profile means the last run was killed hard
	// (dev stop button, sudden power loss) — its GPU/shader/code caches may be
	// half-written, and a corrupt entry there can crash-loop the GPU process on
	// the NEXT boot (WebGL gone → the map renders nothing). Those caches are
	// cheap to rebuild, so they're dropped; the HTTP cache (map tiles) is kept.
	static constexpr wchar_t kCleanExitMarker[] = L".clean_exit";

	static void dropVolatileCachesAfterUncleanExit(const std::wstring& cacheDir)
	{
		namespace fs = std::filesystem;
		static const std::set<std::wstring> kVolatile = {
			L"GPUCache", L"ShaderCache", L"GrShaderCache",
			L"GraphiteDawnCache", L"DawnCache", L"DawnWebGPUCache",
			L"Code Cache",
		};
		std::vector<fs::path> doomed;
		std::error_code ec;
		for (fs::recursive_directory_iterator it(cacheDir, ec), end;
		     !ec && it != end; it.increment(ec)) {
			if (it->is_directory(ec) &&
			    kVolatile.count(it->path().filename().wstring()) != 0) {
				doomed.push_back(it->path());
				it.disable_recursion_pending();
			}
		}
		for (const auto& dir : doomed) {
			std::error_code rmec;
			fs::remove_all(dir, rmec);
		}
		if (!doomed.empty()) {
			fprintf(stderr,
			        "[webview_cef] previous run exited uncleanly; dropped %zu "
			        "GPU/shader/code cache dir(s) to avoid a poisoned GPU "
			        "process.\n",
			        doomed.size());
			fflush(stderr);
		}
	}
#endif
#ifdef OS_MAC
	std::string g_macSubprocessPath;
	std::string g_macFrameworkDirPath;
	std::string g_macMainBundlePath;

	void setMacCEFPaths(const std::string& subprocessPath,
	                    const std::string& frameworkDirPath,
	                    const std::string& mainBundlePath) {
		g_macSubprocessPath = subprocessPath;
		g_macFrameworkDirPath = frameworkDirPath;
		g_macMainBundlePath = mainBundlePath;
	}
#endif

	WebviewPlugin::WebviewPlugin() {
		m_handler = new WebviewHandler();
        }

    WebviewPlugin::~WebviewPlugin() {
		uninitCallback();
		m_handler->CloseAllBrowsers(true);
		m_handler = nullptr;
		if(!m_renderers.empty()){
			m_renderers.clear();
		}
	}

	void WebviewPlugin::initCallback() {
		if (!m_init)
		{
			m_handler->onPaintCallback = [=, this](int browserId, const void* buffer, int32_t width, int32_t height) {
				if (m_renderers.find(browserId) != m_renderers.end() && m_renderers[browserId] != nullptr) {
					m_renderers[browserId]->onFrame(buffer, width, height);
				}
			};

			m_handler->onAcceleratedPaintCallback = [=, this](int browserId, const void* sharedHandle, int32_t width, int32_t height, int32_t format) {
				if (m_renderers.find(browserId) != m_renderers.end() && m_renderers[browserId] != nullptr) {
					m_renderers[browserId]->onAcceleratedFrame(sharedHandle, width, height, format);
				}
			};

			m_handler->onTooltipEvent = [=, this](int browserId, std::string text) {
				if (m_invokeFunc) {
					WValue* bId = webview_value_new_int(browserId);
					WValue* wText = webview_value_new_string(const_cast<char*>(text.c_str()));
					WValue* retMap = webview_value_new_map();
					webview_value_set_string(retMap, "browserId", bId);
					webview_value_set_string(retMap, "text", wText);
					m_invokeFunc("onTooltip", retMap);
					webview_value_unref(bId);
					webview_value_unref(wText);
					webview_value_unref(retMap);
				}
			};

			m_handler->onCursorChangedEvent = [=, this](int browserId, int type) {
				if(m_invokeFunc){
					WValue* bId = webview_value_new_int(browserId);
					WValue* wType = webview_value_new_int(type);
					WValue* retMap = webview_value_new_map();
					webview_value_set_string(retMap, "browserId", bId);
					webview_value_set_string(retMap, "type", wType);
					m_invokeFunc("onCursorChanged", retMap);
					webview_value_unref(bId);
					webview_value_unref(wType);
					webview_value_unref(retMap);
				}
			};

			m_handler->onConsoleMessageEvent = [=, this](int browserId, int level, std::string message, std::string source, int line){
				if(m_invokeFunc){
					WValue* bId = webview_value_new_int(browserId);
					WValue* wLevel = webview_value_new_int(level);
					WValue* wMessage = webview_value_new_string(const_cast<char*>(message.c_str()));
					WValue* wSource = webview_value_new_string(const_cast<char*>(source.c_str()));
					WValue* wLine = webview_value_new_int(line);
					WValue* retMap = webview_value_new_map();
					webview_value_set_string(retMap, "browserId", bId);
					webview_value_set_string(retMap, "level", wLevel);
					webview_value_set_string(retMap, "message", wMessage);
					webview_value_set_string(retMap, "source", wSource);
					webview_value_set_string(retMap, "line", wLine);
					m_invokeFunc("onConsoleMessage", retMap);
					webview_value_unref(bId);
					webview_value_unref(wLevel);
					webview_value_unref(wMessage);
					webview_value_unref(wSource);
					webview_value_unref(wLine);
					webview_value_unref(retMap);
				}
			};

			m_handler->onUrlChangedEvent = [=, this](int browserId, std::string url)
			{
				if (m_invokeFunc)
				{
					WValue* bId = webview_value_new_int(browserId);
					WValue* wUrl = webview_value_new_string(const_cast<char*>(url.c_str()));
					WValue* retMap = webview_value_new_map();
					webview_value_set_string(retMap, "browserId", bId);
					webview_value_set_string(retMap, "url", wUrl);
					m_invokeFunc("urlChanged", retMap);
					webview_value_unref(bId);
					webview_value_unref(wUrl);
					webview_value_unref(retMap);
				}
			};

			m_handler->onTitleChangedEvent = [=, this](int browserId, std::string title)
			{
				if (m_invokeFunc)
				{
					WValue* bId = webview_value_new_int(browserId);
					WValue* wTitle = webview_value_new_string(const_cast<char*>(title.c_str()));
					WValue* retMap = webview_value_new_map();
					webview_value_set_string(retMap, "browserId", bId);
					webview_value_set_string(retMap, "title", wTitle);
					m_invokeFunc("titleChanged", retMap);
					webview_value_unref(bId);
					webview_value_unref(wTitle);
					webview_value_unref(retMap);
				}
			};

			m_handler->onJavaScriptChannelMessage = [=, this](std::string channelName, std::string message, std::string callbackId, int browserId, std::string frameId)
			{
				if (m_invokeFunc)
				{
					WValue* retMap = webview_value_new_map();
					WValue* channel = webview_value_new_string(const_cast<char*>(channelName.c_str()));
					WValue* msg = webview_value_new_string(const_cast<char*>(message.c_str()));
					WValue* cbId = webview_value_new_string(const_cast<char*>(callbackId.c_str()));
					WValue* bId = webview_value_new_int(browserId);
					WValue* fId = webview_value_new_string(const_cast<char*>(frameId.c_str()));
					webview_value_set_string(retMap, "channel", channel);
					webview_value_set_string(retMap, "message", msg);
					webview_value_set_string(retMap, "callbackId", cbId);
					webview_value_set_string(retMap, "browserId", bId);
					webview_value_set_string(retMap, "frameId", fId);
					m_invokeFunc("javascriptChannelMessage", retMap);
					webview_value_unref(retMap);
					webview_value_unref(channel);
					webview_value_unref(msg);
					webview_value_unref(cbId);
					webview_value_unref(bId);
					webview_value_unref(fId);
				}
			};

			m_handler->onFocusedNodeChangeMessage = [=, this](int nBrowserId, bool bEditable)
			{
				// Track editable focus per browser so the platform layer can route
				// raw character keys to the OS IME while a web input is focused
				// (the IME/delta path handles text). Focus moving to a new node
				// also ends any prior composition for that browser.
				auto rit = m_renderers.find(nBrowserId);
				if (rit != m_renderers.end() && rit->second) {
					rit->second->editableFocused = bEditable;
					rit->second->composing = false;
				}
				if (m_invokeFunc)
				{
					WValue* bId = webview_value_new_int(int64_t(nBrowserId));
					WValue* editable = webview_value_new_bool(bEditable);
					WValue* retMap = webview_value_new_map();
					webview_value_set_string(retMap, "browserId", bId);
					webview_value_set_string(retMap, "editable", editable);
					m_invokeFunc("onFocusedNodeChangeMessage", retMap);
					webview_value_unref(bId);
					webview_value_unref(editable);
					webview_value_unref(retMap);
				}
			};

			m_handler->onImeCompositionRangeChangedMessage = [=, this](int nBrowserId, int32_t x, int32_t y, int32_t height)
			{
				if (m_invokeFunc)
				{
					WValue* bId = webview_value_new_int(nBrowserId);
					WValue* retMap = webview_value_new_map();
					WValue* xValue = webview_value_new_int(x);
					WValue* yValue = webview_value_new_int(y);
					WValue* hValue = webview_value_new_int(height);
					webview_value_set_string(retMap, "browserId", bId);
					webview_value_set_string(retMap, "x", xValue);
					webview_value_set_string(retMap, "y", yValue);
					webview_value_set_string(retMap, "height", hValue);
					m_invokeFunc("onImeCompositionRangeChangedMessage", retMap);
					webview_value_unref(bId);
					webview_value_unref(xValue);
					webview_value_unref(yValue);
					webview_value_unref(hValue);
					webview_value_unref(retMap);
				}
			};


            m_handler->onLoadStart = [=, this](int nBrowserId, std::string urlId)
            {
                if (m_invokeFunc)
                {
                    WValue* bId = webview_value_new_int(nBrowserId);
                    WValue* uId = webview_value_new_string(const_cast<char*>(urlId.c_str()));
                    WValue* retMap = webview_value_new_map();
                    webview_value_set_string(retMap, "browserId", bId);
                    webview_value_set_string(retMap, "urlId", uId);
                    m_invokeFunc("onLoadStart", retMap);
                    webview_value_unref(bId);
                    webview_value_unref(uId);
                    webview_value_unref(retMap);
                }
            };

            m_handler->onLoadEnd = [=, this](int nBrowserId, std::string urlId)
            {
                if (m_invokeFunc)
                {
                    WValue* bId = webview_value_new_int(nBrowserId);
                    WValue* uId = webview_value_new_string(const_cast<char*>(urlId.c_str()));
                    WValue* retMap = webview_value_new_map();
                    webview_value_set_string(retMap, "browserId", bId);
                    webview_value_set_string(retMap, "urlId", uId);
                    m_invokeFunc("onLoadEnd", retMap);
                    webview_value_unref(bId);
                    webview_value_unref(uId);
                    webview_value_unref(retMap);
                }
            };

			m_init = true;
		}
	}

	void WebviewPlugin::uninitCallback(){
		m_handler->onPaintCallback = nullptr;
		m_handler->onAcceleratedPaintCallback = nullptr;
		m_handler->onTooltipEvent = nullptr;
		m_handler->onCursorChangedEvent = nullptr;
		m_handler->onConsoleMessageEvent = nullptr;
		m_handler->onUrlChangedEvent = nullptr;
		m_handler->onTitleChangedEvent = nullptr;
		m_handler->onJavaScriptChannelMessage = nullptr;
		m_handler->onFocusedNodeChangeMessage = nullptr;
		m_handler->onImeCompositionRangeChangedMessage = nullptr;
		m_init = false;
	}


    void WebviewPlugin::HandleMethodCall(std::string name, WValue* values, std::function<void(int ,WValue*)> result) {
		if (name.compare("init") == 0){
			if(!isCefInitialized){
				if(values != nullptr){
					userAgent = CefString(webview_value_get_string(values));
				}
				startCEF();
			}
			initCallback();
			result(1, nullptr);
		}
		else if (name.compare("quit") == 0) {
			//only call this method when you want to quit the app
			stopCEF();
			result(1, nullptr);
		}
		else if (name.compare("create") == 0) {
			std::string url = webview_value_get_string(values);
			m_handler->createBrowser(url, [=, this](int browserId) {
				std::shared_ptr<WebviewTexture> renderer = m_createTextureFunc();
				m_renderers[browserId] = renderer;
				WValue	*response = webview_value_new_list();
				webview_value_append(response, webview_value_new_int(browserId));
				webview_value_append(response, webview_value_new_int(renderer->textureId));
				result(1, response);
				webview_value_unref(response);
			});
		}
		else if (name.compare("close") == 0) {
			int browserId = int(webview_value_get_int(values));
			m_handler->closeBrowser(browserId);
			if(m_renderers.find(browserId) != m_renderers.end() && m_renderers[browserId] != nullptr) {
				m_renderers[browserId].reset();
			}
			result(1, nullptr);
		}
		else if (name.compare("loadUrl") == 0) {
			int browserId = int(webview_value_get_int(webview_value_get_list_value(values, 0)));
			const auto url = webview_value_get_string(webview_value_get_list_value(values, 1));
			if(url != nullptr){
				m_handler->loadUrl(browserId, url);
				result(1, nullptr);
			}
		}
		else if (name.compare("setSize") == 0) {
			int browserId = int(webview_value_get_int(webview_value_get_list_value(values, 0)));
			const auto dpi = webview_value_get_double(webview_value_get_list_value(values, 1));
			const auto width = webview_value_get_double(webview_value_get_list_value(values, 2));
			const auto height = webview_value_get_double(webview_value_get_list_value(values, 3));
			m_handler->changeSize(browserId, (float)dpi, (int)std::round(width), (int)std::round(height));
			result(1, nullptr);
		}
		else if (name.compare("cursorClickDown") == 0 
			|| name.compare("cursorClickUp") == 0 
			|| name.compare("cursorMove") == 0 
			|| name.compare("cursorDragging") == 0) {
			result(cursorAction(values, name), nullptr);
		}
		else if (name.compare("setScrollDelta") == 0) {
			// Deltas arrive as unscaled doubles from Dart (see webview.dart);
			// tolerate ints for any caller still sending the legacy encoding.
			auto asDouble = [](WValue* v) -> double {
				switch (webview_value_get_type(v)) {
					case Webview_Value_Type_Double:
						return webview_value_get_double(v);
					case Webview_Value_Type_Float:
						return (double)webview_value_get_float(v);
					default:
						return (double)webview_value_get_int(v);
				}
			};
			int browserId = int(webview_value_get_int(webview_value_get_list_value(values, 0)));
			auto x = webview_value_get_int(webview_value_get_list_value(values, 1));
			auto y = webview_value_get_int(webview_value_get_list_value(values, 2));
			double deltaX = asDouble(webview_value_get_list_value(values, 3));
			double deltaY = asDouble(webview_value_get_list_value(values, 4));
			m_handler->sendScrollEvent(browserId, (int)x, (int)y, deltaX, deltaY);
			result(1, nullptr);
		}
		else if (name.compare("sendTouchEvent") == 0) {
			// Args: [browserId, id, phase, x, y, pressure]. Phase mapping shared
			// with the Dart layer: 0=down, 1=move, 2=up, 3=cancel. Positions and
			// pressure arrive as doubles (tolerate ints like setScrollDelta does).
			auto asDouble = [](WValue* v) -> double {
				switch (webview_value_get_type(v)) {
					case Webview_Value_Type_Double:
						return webview_value_get_double(v);
					case Webview_Value_Type_Float:
						return (double)webview_value_get_float(v);
					default:
						return (double)webview_value_get_int(v);
				}
			};
			int browserId = int(webview_value_get_int(webview_value_get_list_value(values, 0)));
			int id = int(webview_value_get_int(webview_value_get_list_value(values, 1)));
			int phase = int(webview_value_get_int(webview_value_get_list_value(values, 2)));
			double x = asDouble(webview_value_get_list_value(values, 3));
			double y = asDouble(webview_value_get_list_value(values, 4));
			double pressure = asDouble(webview_value_get_list_value(values, 5));
			m_handler->sendTouchEvent(browserId, id, phase, x, y, pressure);
			result(1, nullptr);
		}
		else if (name.compare("goForward") == 0) {
			int browserId = int(webview_value_get_int(values));
			m_handler->goForward(browserId);
			result(1, nullptr);
		}
		else if (name.compare("goBack") == 0) {
			int browserId = int(webview_value_get_int(values));
			m_handler->goBack(browserId);
			result(1, nullptr);
		}
		else if (name.compare("reload") == 0) {
			int browserId = int(webview_value_get_int(values));
			m_handler->reload(browserId);
			result(1, nullptr);
		}
		else if (name.compare("openDevTools") == 0) {			
			int browserId = int(webview_value_get_int(values));
			m_handler->openDevTools(browserId);
			result(1, nullptr);
		}
		else if (name.compare("imeSetComposition") == 0) {
			int browserId = int(webview_value_get_int(webview_value_get_list_value(values, 0)));
			const auto text = webview_value_get_string(webview_value_get_list_value(values, 1));
			// Non-empty preedit → composition active; empty preedit means the IME
			// cleared it (e.g. the user deleted the last composing letter).
			setComposingForBrowser(browserId, text != nullptr && text[0] != '\0');
			m_handler->imeSetComposition(browserId, text);
			result(1, nullptr);
		} 
		else if (name.compare("imeCommitText") == 0) {
			int browserId = int(webview_value_get_int(webview_value_get_list_value(values, 0)));
			const auto text = webview_value_get_string(webview_value_get_list_value(values, 1));
			// Commit ends the active composition.
			setComposingForBrowser(browserId, false);
			m_handler->imeCommitText(browserId, text);
			result(1, nullptr);
		} 
		else if (name.compare("setClientFocus") == 0) {
			int browserId = int(webview_value_get_int(webview_value_get_list_value(values, 0)));
			if (m_renderers.find(browserId) != m_renderers.end() && m_renderers[browserId] != nullptr) {
				bool focused = webview_value_get_bool(webview_value_get_list_value(values, 1));
				m_renderers[browserId].get()->isFocused = focused;
				// Losing focus ends any composition: the OS IME drops marked text,
				// but CEF's SetFocus(false) does not blur the DOM node, so no
				// FocusedNodeChanged fires to clear it — a stale flag would then
				// mis-route the first keys after the webview is refocused.
				if (!focused) {
					m_renderers[browserId].get()->composing = false;
				}
				m_handler->setClientFocus(browserId, focused);
			}
			result(1, nullptr);
		}
		else if(name.compare("setCookie") == 0){
			const auto domain = webview_value_get_string(webview_value_get_list_value(values, 0));
			const auto key = webview_value_get_string(webview_value_get_list_value(values, 1));
			const auto value = webview_value_get_string(webview_value_get_list_value(values, 2));
			m_handler->setCookie(domain, key, value);
			result(1, nullptr);
		}
		else if (name.compare("deleteCookie") == 0) {
			const auto domain = webview_value_get_string(webview_value_get_list_value(values, 0));
			const auto key = webview_value_get_string(webview_value_get_list_value(values, 1));
			m_handler->deleteCookie(domain, key);
			result(1, nullptr);
		}
		else if (name.compare("visitAllCookies") == 0) {
			m_handler->visitAllCookies([=](std::map<std::string, std::map<std::string, std::string>> cookies){
				WValue* retMap = webview_value_new_map();
				for (auto &cookie : cookies)
				{
					WValue* tempMap = webview_value_new_map();
					for (auto &c : cookie.second)
					{
						WValue * val = webview_value_new_string(const_cast<char *>(c.second.c_str()));
						webview_value_set_string(tempMap, c.first.c_str(), val);
						webview_value_unref(val);
					}
					webview_value_set_string(retMap, cookie.first.c_str(), tempMap);
					webview_value_unref(tempMap);
				}
				result(1, retMap);	
				webview_value_unref(retMap);
			});
		}
		else if (name.compare("visitUrlCookies") == 0) {
			const auto domain = webview_value_get_string(webview_value_get_list_value(values, 0));
			const auto isHttpOnly = webview_value_get_bool(webview_value_get_list_value(values, 1));
			m_handler->visitUrlCookies(domain, isHttpOnly,[=](std::map<std::string, std::map<std::string, std::string>> cookies){
				WValue* retMap = webview_value_new_map();
				for (auto &cookie : cookies)
				{
					WValue* tempMap = webview_value_new_map();
					for (auto &c : cookie.second)
					{
						WValue * val = webview_value_new_string(const_cast<char *>(c.second.c_str()));
						webview_value_set_string(tempMap, c.first.c_str(), val);
						webview_value_unref(val);
					}
					webview_value_set_string(retMap, cookie.first.c_str(), tempMap);
					webview_value_unref(tempMap);
				}
				result(1, retMap);	
				webview_value_unref(retMap);
			});
		}
		else if(name.compare("setJavaScriptChannels") == 0){
			int browserId = int(webview_value_get_int(webview_value_get_list_value(values, 0)));
			WValue *list = webview_value_get_list_value(values, 1);
			auto len = webview_value_get_len(list);
			std::vector<std::string> channels;
			for(size_t i = 0; i < len; i++){
				auto channel = webview_value_get_string(webview_value_get_list_value(list, i));
				channels.push_back(channel);
			}
			m_handler->setJavaScriptChannels(browserId, channels);
			result(1, nullptr);
		}
		else if (name.compare("sendJavaScriptChannelCallBack") == 0) {
			const auto error = webview_value_get_bool(webview_value_get_list_value(values, 0));
			const auto ret = webview_value_get_string(webview_value_get_list_value(values, 1));
			const auto callbackId = webview_value_get_string(webview_value_get_list_value(values, 2));
			const auto browserId = int(webview_value_get_int(webview_value_get_list_value(values, 3)));
			const auto frameId = webview_value_get_string(webview_value_get_list_value(values, 4));
			m_handler->sendJavaScriptChannelCallBack(error, ret, callbackId, browserId, frameId);
			result(1, nullptr);
		}
		else if (name.compare("sendKeyEvent") == 0) {
			int type = int(webview_value_get_int(webview_value_get_list_value(values, 1)));
			int keyCode = int(webview_value_get_int(webview_value_get_list_value(values, 2)));
			int modifiers = int(webview_value_get_int(webview_value_get_list_value(values, 3)));
			int character = int(webview_value_get_int(webview_value_get_list_value(values, 4)));
			int unmodifiedCharacter = int(webview_value_get_int(webview_value_get_list_value(values, 5)));
			
			CefKeyEvent keyEvent;
			keyEvent.type = static_cast<cef_key_event_type_t>(type);
			keyEvent.windows_key_code = keyCode;
			keyEvent.modifiers = modifiers;
			keyEvent.character = static_cast<char16_t>(character);
			keyEvent.unmodified_character = static_cast<char16_t>(unmodifiedCharacter);
			
			sendKeyEvent(keyEvent);
			result(1, nullptr);
		}
		else if(name.compare("hasNativeKeySupport") == 0) {
#if defined(HAS_GTK) || defined(OS_WIN) || defined(OS_MAC)
			// Desktop Linux (GTK via processKeyEventForCEF), Windows (WM_KEYDOWN)
			// and macOS (NSEventMaskKeyDown) all deliver keys to CEF natively.
			bool hasNativeKeySupport = true;
#else
			// eLinux (no GTK): use Dart-side handling.
			bool hasNativeKeySupport = false;
#endif
			WValue* ret = webview_value_new_bool(hasNativeKeySupport);
			result(1, ret);
			webview_value_unref(ret);
		}
		else if(name.compare("executeJavaScript") == 0){
			int browserId = int(webview_value_get_int(webview_value_get_list_value(values, 0)));
			const auto code = webview_value_get_string(webview_value_get_list_value(values, 1));
			m_handler->executeJavaScript(browserId, code);
			result(1, nullptr);
		}
		else if(name.compare("evaluateJavascript") == 0){
			int browserId = int(webview_value_get_int(webview_value_get_list_value(values, 0)));
			const auto code = webview_value_get_string(webview_value_get_list_value(values, 1));
			m_handler->executeJavaScript(browserId, code, [=](CefRefPtr<CefValue> values){
                WValue* retValue;

                if (values == nullptr) {
                    result(1, nullptr);
                    return;
                }

                switch(values->GetType()) {
                    case VTYPE_BOOL:
                        retValue = webview_value_new_bool(values->GetBool());
                        break;
                    case VTYPE_DOUBLE:
                        retValue = webview_value_new_double(values->GetDouble());
                        break;
                    case VTYPE_INT:
                        retValue = webview_value_new_int(values->GetInt());
                        break;
                    case VTYPE_STRING:
                        retValue = webview_value_new_string(values->GetString().ToString().c_str());
                        break;
                    case VTYPE_LIST: {
                        retValue = webview_value_new_list();
                        CefRefPtr<CefListValue> list = values->GetList();
                        
                        if (list) {
                            for (size_t i = 0; i < list->GetSize(); ++i) {
                                CefValueType type = list->GetType(i);
                                CefRefPtr<CefValue> listItem = list->GetValue(i);

                                if (type == VTYPE_INT) {
                                    webview_value_append(retValue, webview_value_new_int(listItem->GetInt()));
                                } else if (type == VTYPE_BOOL) {
                                    webview_value_append(retValue, webview_value_new_bool(listItem->GetBool()));
                                } else if (type == VTYPE_STRING) {
                                    webview_value_append(retValue, webview_value_new_string(listItem->GetString().ToString().c_str()));
                                } else if (type == VTYPE_DOUBLE) {
                                    webview_value_append(retValue, webview_value_new_double(listItem->GetDouble()));
                                } else {
                                    continue;
                                }
                            }
                        }
                        break;
                    }
                    default:
                        // Return null as fallback
                        result(1, nullptr);
                        return;
                }

				result(1, retValue);
				webview_value_unref(retValue);
			});
		}
		else {
			result = 0;
		}
	}

	void WebviewPlugin::imeSetCompositionNative(const std::wstring& text, int cursor)
	{
		m_handler->imeSetCompositionNative(text, cursor);
	}

	void WebviewPlugin::imeCommitTextNative(const std::wstring& text)
	{
		m_handler->imeCommitTextNative(text);
	}

	void WebviewPlugin::imeFinishCompositionNative()
	{
		m_handler->imeFinishComposition();
	}

	void WebviewPlugin::sendKeyEvent(CefKeyEvent& ev)
	{
		m_handler->sendKeyEvent(ev);
		if(ev.type == KEYEVENT_RAWKEYDOWN && ev.windows_key_code == 0x7B && (ev.modifiers & EVENTFLAG_CONTROL_DOWN) != 0){
			for(auto render : m_renderers){
				if(render.second.get()->isFocused){
					m_handler->openDevTools(render.first);
				}
			}
		}
	}

	void WebviewPlugin::setInvokeMethodFunc(std::function<void(std::string, WValue*)> func){
		m_invokeFunc = func;
	}

	void WebviewPlugin::setCreateTextureFunc(std::function<std::shared_ptr<WebviewTexture>()> func)
	{
		m_createTextureFunc = func;
	}
	
	bool WebviewPlugin::getAnyBrowserFocused(){
		for(auto render : m_renderers){
			if(render.second != nullptr && render.second.get()->isFocused){
				return true;
			}
		}
		return false;
	}

	int WebviewPlugin::focusedBrowserId(){
		for(auto& render : m_renderers){
			if(render.second != nullptr && render.second->isFocused){
				return render.first;
			}
		}
		return -1;
	}

	bool WebviewPlugin::isEditableFocused(){
		int id = focusedBrowserId();
		if(id < 0){
			return false;
		}
		auto it = m_renderers.find(id);
		return it != m_renderers.end() && it->second && it->second->editableFocused;
	}

	bool WebviewPlugin::isComposing(){
		int id = focusedBrowserId();
		if(id < 0){
			return false;
		}
		auto it = m_renderers.find(id);
		return it != m_renderers.end() && it->second && it->second->composing;
	}

	void WebviewPlugin::setComposingForBrowser(int browserId, bool composing){
		auto it = m_renderers.find(browserId);
		if(it != m_renderers.end() && it->second){
			it->second->composing = composing;
		}
	}

	void WebviewPlugin::tickBeginFrame(){
		if (m_handler) {
			m_handler->sendExternalBeginFrame();
		}
	}
	
	int WebviewPlugin::cursorAction(WValue *args, std::string name) {
		if (!args || webview_value_get_len(args) != 3) {
			return 0;
		}
		int browserId = int(webview_value_get_int(webview_value_get_list_value(args, 0)));
		int x = int(webview_value_get_int(webview_value_get_list_value(args, 1)));
		int y = int(webview_value_get_int(webview_value_get_list_value(args, 2)));
		if (!x && !y) {
			return 0;
		}
		if (name.compare("cursorClickDown") == 0) {
			m_handler->cursorClick(browserId, x, y, false);
		}
		else if (name.compare("cursorClickUp") == 0) {
			m_handler->cursorClick(browserId, x, y, true);
		}
		else if (name.compare("cursorMove") == 0) {
			m_handler->cursorMove(browserId, x, y, false);
		}
		else if (name.compare("cursorDragging") == 0) {
			m_handler->cursorMove(browserId, x, y, true);
		}
		return 1;
	}

	int initCEFProcesses(CefMainArgs args)
	{
		mainArgs = args;
		return initCEFProcesses();
	}

	int initCEFProcesses()
	{
#ifdef OS_MAC
		CefScopedLibraryLoader loader;
		if(!loader.LoadInMain()) {
			printf("load cef err");
		}
#endif
		// handler = new WebviewHandler();
		app = new WebviewApp();
#ifdef OS_MAC
		// No bundled helper resolved → fall back to single-process so consumers
		// that haven't added the CEF helper apps keep working (mode 3 appends
		// the "single-process" switch in OnBeforeCommandLineProcessing).
		if (g_macSubprocessPath.empty()) {
			app->SetProcessMode(3);
		}
#endif
		return CefExecuteProcess(mainArgs, app, nullptr);
	}

#ifdef _WIN32
	// Default persistent profile location: %LOCALAPPDATA%\<exe name>\webview_cef.
	// Keyed per executable so different embedder apps never share a profile
	// (CEF locks root_cache_path to a single running browser-process instance).
	// Returns empty if the location can't be resolved.
	static std::wstring defaultCachePath()
	{
		wchar_t exePath[MAX_PATH];
		const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
		if (len == 0 || len >= MAX_PATH) {
			return L"";
		}
		std::wstring name(exePath, len);
		const size_t slash = name.find_last_of(L"\\/");
		if (slash != std::wstring::npos) {
			name = name.substr(slash + 1);
		}
		const size_t dot = name.find_last_of(L'.');
		if (dot != std::wstring::npos) {
			name = name.substr(0, dot);
		}
		wchar_t* localAppData = nullptr;
		size_t envLen = 0;
		if (_wdupenv_s(&localAppData, &envLen, L"LOCALAPPDATA") != 0 || localAppData == nullptr) {
			return L"";
		}
		std::wstring base(localAppData);
		free(localAppData);
		if (base.empty() || name.empty()) {
			return L"";
		}
		return base + L"\\" + name + L"\\webview_cef";
	}
#endif

	void startCEF()
	{
		CefSettings cefs;
		cefs.windowless_rendering_enabled = true;
		cefs.no_sandbox = true;
#ifdef _WIN32
		// Persistent browser profile. Without cache_path CEF runs a fully
		// in-memory ("incognito") profile: no HTTP cache (map tiles, glyphs,
		// sprites re-download every launch) and nowhere to keep the GPU shader
		// disk cache, so WebGL-heavy pages recompile every shader on each boot.
		// root_cache_path is left empty and defaults to cache_path.
		const std::wstring cachePath = defaultCachePath();
		if (!cachePath.empty()) {
			namespace fs = std::filesystem;
			std::error_code ec;
			fs::create_directories(cachePath, ec);
			if (!ec) {
				// Unclean-exit recovery: no clean-exit marker on a non-empty
				// profile → the previous run was killed hard; drop the
				// volatile caches before CEF touches them.
				const fs::path marker = fs::path(cachePath) / kCleanExitMarker;
				std::error_code mec;
				const bool cleanExit = fs::exists(marker, mec);
				const bool hasContents =
				    fs::directory_iterator(cachePath, mec) !=
				    fs::directory_iterator();
				if (hasContents && !cleanExit) {
					dropVolatileCachesAfterUncleanExit(cachePath);
				}
				// This run is dirty until stopCEF() completes.
				fs::remove(marker, mec);

				CefString(&cefs.cache_path).FromWString(cachePath);
				g_cacheDir = cachePath;
				// Keeps the GPU shader disk cache enabled (see
				// WebviewApp::OnBeforeCommandLineProcessing).
				if (app) {
					app->SetHasPersistentCache(true);
				}
			}
		}
#endif
		if(!userAgent.empty()){
			CefString(&cefs.user_agent_product) = userAgent;
		}
		//locale language setting
		//CefString(&cefs.locale) = "zh-CN";
#ifdef OS_MAC
		//cef message loop handle by MainApplication on mac
		cefs.external_message_pump = true;
		// Multi-process: point CEF at the bundled helper executable and the
		// framework. The platform layer fills these from the app bundle.
		if (!g_macSubprocessPath.empty()) {
			CefString(&cefs.browser_subprocess_path) = g_macSubprocessPath;
		}
		if (!g_macFrameworkDirPath.empty()) {
			CefString(&cefs.framework_dir_path) = g_macFrameworkDirPath;
		}
		if (!g_macMainBundlePath.empty()) {
			CefString(&cefs.main_bundle_path) = g_macMainBundlePath;
		}
#else
		//cef message run in another thread on windows/linux
		cefs.multi_threaded_message_loop = true;
#endif
		g_cefInitOk = CefInitialize(mainArgs, cefs, app.get(), nullptr);
		if (!g_cefInitOk) {
			// Most likely cause with a persistent cache_path: another (possibly
			// zombie) instance still holds the profile lock. Without this log the
			// failure is invisible — browsers just never create and the map stays
			// uncontrollable/blank.
			fprintf(stderr,
			        "[webview_cef] ERROR: CefInitialize failed (exit code %d). "
			        "If a previous instance is still running (or died without "
			        "releasing the profile lock), close it or delete the cache "
			        "directory. Webviews will not work in this session.\n",
			        CefGetExitCode());
			fflush(stderr);
		}
	}

	void doMessageLoopWork(){
		CefDoMessageLoopWork();
	}

	void SwapBufferFromBgraToRgba(void* _dest, const void* _src, int width, int height) {
		int32_t* dest = (int32_t*)_dest;
		int32_t* src = (int32_t*)_src;
		int32_t rgba;
		int32_t bgra;
		int length = width * height;
		for (int i = 0; i < length; i++) {
			bgra = src[i];
			// BGRA in hex = 0xAARRGGBB.
			rgba = (bgra & 0x00ff0000) >> 16 // Red >> Blue.
				| (bgra & 0xff00ff00) // Green Alpha.
				| (bgra & 0x000000ff) << 16; // Blue >> Red.
			dest[i] = rgba;
		}
	}

    void stopCEF()
    {
		// CefShutdown without a successful CefInitialize crashes.
		if (!g_cefInitOk) {
			return;
		}
		// Browser closes are async (the plugin dtor's CloseAllBrowsers posts to
		// CEF's UI thread, which runs on its own thread here —
		// multi_threaded_message_loop). CefShutdown while any browser is alive
		// is undefined: it can hang the process invisibly after the window
		// closed, and the zombie's half-written profile then breaks every
		// subsequent launch. Wait (bounded) for the closes to land first.
		for (int i = 0; i < 200 && WebviewHandler::liveBrowserCount() > 0; ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (WebviewHandler::liveBrowserCount() > 0) {
			fprintf(stderr,
			        "[webview_cef] WARNING: browsers still open after 2s; "
			        "forcing CefShutdown anyway.\n");
			fflush(stderr);
		}
		CefShutdown();
		g_cefInitOk = false;
#ifdef _WIN32
		// Mark this run as cleanly shut down (see kCleanExitMarker).
		if (!g_cacheDir.empty()) {
			std::ofstream(std::filesystem::path(g_cacheDir) / kCleanExitMarker)
			    << '1';
		}
#endif
    }
}
