import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:webview_cef/webview_cef.dart';

/// Windows promotes every touch contact to mouse messages too; one of them can
/// reach the plugin as a mouse press whose release never comes. Forwarded to
/// CEF that press stays held and every later touch drag fights it. These
/// tests drive the webview's pointer routing and check what reaches CEF.
void main() {
  const channel = MethodChannel('webview_cef');
  final calls = <MethodCall>[];

  setUp(() {
    calls.clear();
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      if (call.method == 'create') return [7, 1];
      return null;
    });
  });

  Future<void> pumpWebView(WidgetTester tester) async {
    final controller = WebviewManager().createWebView();
    await tester.runAsync(() async {
      await WebviewManager().initialize();
      await controller.initialize('about:blank');
    });
    await tester.pumpWidget(
      MaterialApp(home: Scaffold(body: controller.webviewWidget)),
    );
    calls.clear();
  }

  List<String> sent(String method) => [
        for (final c in calls)
          if (c.method == method) '${(c.arguments as List).skip(1).take(2)}',
      ];

  Duration ms(int v) => Duration(milliseconds: v);

  testWidgets('a mouse press during a touch never reaches CEF', (tester) async {
    await pumpWebView(tester);
    final finger = await tester.createGesture(kind: PointerDeviceKind.touch);
    final mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);

    await finger.down(const Offset(100, 100), timeStamp: ms(1000));
    await mouse.down(const Offset(100, 100), timeStamp: ms(1010));
    await mouse.moveTo(const Offset(150, 100), timeStamp: ms(1020));
    await finger.moveTo(const Offset(150, 100), timeStamp: ms(1020));
    await finger.up(timeStamp: ms(1100));
    await mouse.up(timeStamp: ms(1110));
    await tester.pump(ms(100));

    expect(sent('cursorClickDown'), isEmpty);
    expect(sent('cursorDragging'), isEmpty);
    expect(sent('cursorClickUp'), isEmpty);
    expect(sent('sendTouchEvent'), hasLength(3));
  });

  testWidgets(
      'a mouse press right after a touch is dropped, a later one '
      'goes through', (tester) async {
    await pumpWebView(tester);
    final finger = await tester.createGesture(kind: PointerDeviceKind.touch);
    final mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);

    await finger.down(const Offset(100, 100), timeStamp: ms(1000));
    await finger.up(timeStamp: ms(1100));
    await mouse.down(const Offset(100, 100), timeStamp: ms(1300));
    await mouse.up(timeStamp: ms(1310));
    expect(sent('cursorClickDown'), isEmpty);

    await mouse.down(const Offset(120, 100), timeStamp: ms(1700));
    await mouse.up(timeStamp: ms(1710));
    await tester.pump(ms(100));

    expect(sent('cursorClickDown'), ['(120, 100)']);
    expect(sent('cursorClickUp'), ['(120, 100)']);
  });

  testWidgets('a cancelled mouse press is released in CEF', (tester) async {
    await pumpWebView(tester);
    final mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);

    await mouse.down(const Offset(50, 60), timeStamp: ms(5000));
    await mouse.cancel(timeStamp: ms(5100));
    await tester.pump(ms(100));

    expect(sent('cursorClickDown'), ['(50, 60)']);
    expect(sent('cursorClickUp'), ['(50, 60)']);
  });

  testWidgets(
      'a touch releases a held mouse press where it was, not a pen '
      'press', (tester) async {
    await pumpWebView(tester);
    final mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);
    final pen = await tester.createGesture(kind: PointerDeviceKind.stylus);
    final finger = await tester.createGesture(kind: PointerDeviceKind.touch);

    await mouse.down(const Offset(10, 20), timeStamp: ms(5000));
    await mouse.moveTo(const Offset(30, 20), timeStamp: ms(5010));
    await pen.down(const Offset(200, 200), timeStamp: ms(5020));
    await finger.down(const Offset(300, 300), timeStamp: ms(6000));
    await tester.pump(ms(100));

    expect(sent('cursorClickUp'), ['(30, 20)']);

    // The mouse is still physically held: its next moves must not drag in
    // CEF, which already got the release.
    await finger.up(timeStamp: ms(6100));
    await mouse.moveTo(const Offset(40, 20), timeStamp: ms(7000));
    await tester.pump(ms(100));

    expect(sent('cursorMove'), ['(40, 20)']);
    expect(sent('cursorDragging'), ['(30, 20)']);
    await pen.up(timeStamp: ms(7100));
    await mouse.up(timeStamp: ms(7200));
  });
}
