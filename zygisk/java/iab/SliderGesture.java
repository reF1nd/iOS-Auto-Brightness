// SPDX-License-Identifier: GPL-3.0-only
package iab;

/** One touchscreen contact on one brightness slider. Call finish after the widget handled UP. */
final class SliderGesture {
  // Android may forward events to the expanded panel with the original downTime.
  // The main looper owns every slider in this process.
  private static long menuContact = -1;
  long down = -1;
  private long lastContact = -1;
  private float start;
  private int initial, userValue;
  private boolean moved, released, stopped;

  void begin(long time, float axis, int value) {
    lastContact = time;
    down = time == menuContact ? -1 : time;
    start = axis;
    initial = value;
    userValue = -1;
    moved = released = stopped = false;
  }

  void move(float axis, int slop) { moved |= Math.abs(axis - start) > slop; }
  void progress(int value, boolean fromUser) { if (fromUser) userValue = value; }
  void release() { released = true; }
  void stop() { stopped = released; if (!released) cancel(); }
  void cancel() { down = -1; }
  void openedMenu() {
    if (lastContact >= 0) menuContact = lastContact;
    cancel();
  }

  boolean finish(long contact, int value) {
    if (down != contact || contact < 0) return false;
    boolean commit = contact != menuContact && moved && released && stopped && value == userValue && value != initial;
    cancel();
    return commit;
  }
}
