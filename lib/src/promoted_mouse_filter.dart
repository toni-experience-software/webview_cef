import 'package:flutter/gestures.dart';

/// Tells a real mouse apart from the mouse events Windows synthesizes for a
/// touch.
///
/// Windows promotes every touch contact to mouse messages as well. Flutter
/// drops the ones that carry the touch signature, but a `WM_MOUSEMOVE` without
/// it can slip through while a finger is down, and Flutter then reports a
/// mouse *press* whose release (a signed message) never comes. Forwarded to
/// CEF, that press stays held and every later touch drag fights it.
///
/// So a mouse event is treated as promoted while a touch contact is down and
/// for [window] after the last touch event.
class PromotedMouseFilter {
  PromotedMouseFilter({this.window = const Duration(milliseconds: 500)});

  /// How long after a touch event a mouse event still counts as promoted.
  final Duration window;

  Duration? _lastTouch;

  /// Records a touch event (down, move or up) at [timeStamp].
  void noteTouch(Duration timeStamp) => _lastTouch = timeStamp;

  /// Whether a pointer event of [kind] at [timeStamp] is a promoted copy of a
  /// touch rather than real mouse input. [touchActive] is whether a touch
  /// contact is currently down.
  bool isPromoted(
    PointerDeviceKind kind,
    Duration timeStamp, {
    required bool touchActive,
  }) {
    if (kind != PointerDeviceKind.mouse) return false;
    if (touchActive) return true;
    final last = _lastTouch;
    return last != null && (timeStamp - last).abs() < window;
  }
}
