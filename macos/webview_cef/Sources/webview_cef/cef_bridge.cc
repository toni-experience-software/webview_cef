// Compile the shared C++ implementation once; Package.swift supplies common/.
#ifndef CefBridge_h
#define CefBridge_h

#include "webview_app.cc"
#include "webview_handler.cc"
#include "webview_cookieVisitor.cc"
#include "webview_js_handler.cc"
#include "webview_plugin.cc"
#include "webview_value.cc"
#endif
