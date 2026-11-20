// SPDX-License-Identifier: GPL-3.0-only
package iab;

/** Pair a completed UI contact with an update on the main display handler. */
final class SliderCommit {
  static final long MAX_DELAY_MS = 1500;
  private long contact = -1;
  private int pid;
  private int initialOrder;

  void begin(long down, int source, int order, boolean automatic) {
    contact = automatic ? down : -1;
    pid = source;
    initialOrder = order;
  }

  boolean consume(long down, int source) {
    if (down < 0 || down != contact || source != pid) return false;
    contact = -1;
    return true;
  }

  boolean waiting(int order) { return order == initialOrder; }

  boolean matches(int order, float expected, float lux, float logical) {
    return order == initialOrder + 1 && Float.isFinite(lux) && lux >= 0 && logical > 0
        && Math.abs(expected - logical) <= 0.001f;
  }
}
