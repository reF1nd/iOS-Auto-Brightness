// SPDX-License-Identifier: GPL-3.0-only
package iab;

public final class SliderGestureTest {
  private static void check(boolean pass) {
    if (!pass) throw new AssertionError();
  }
  public static void main(String[] args) {
    SliderCommit match = new SliderCommit();
    match.begin(500, 1001, 8, true);
    check(!match.consume(500, 1002));
    check(!match.consume(501, 1001));
    check(match.consume(500, 1001));
    check(!match.consume(500, 1001));
    check(match.waiting(8));
    check(!match.matches(8, 9, 0, 9));
    check(!match.matches(10, 9, 0, 9));
    check(!match.matches(9, 9, 0, 15));
    check(match.matches(9, 9, 0, 9));
    match.begin(501, 1002, 9, false);
    check(!match.consume(501, 1002));
    SliderGesture g = new SliderGesture();
    // Main-panel long click consumes the whole contact, including a pending preview.
    g.begin(1, 10, 100);
    g.move(30, 8);
    g.progress(101, true);
    g.openedMenu();
    g.release();
    g.stop();
    check(!g.finish(1, 101));
    SliderGesture panel = new SliderGesture();
    panel.begin(1, 10, 100);
    panel.move(30, 8);
    panel.progress(101, true);
    panel.release();
    panel.stop();
    check(!panel.finish(1, 101));
    // New contact in the expanded panel, even for a single brightness step.
    g.begin(2, 10, 100);
    g.move(19, 8);
    g.progress(101, true);
    g.release();
    g.stop();
    check(g.finish(2, 101));
    check(!g.finish(2, 101));
    // A press or jitter can produce fromUser and a stop without a drag.
    g.begin(3, 10, 100);
    g.move(13, 8);
    g.progress(110, true);
    g.release();
    g.stop();
    check(!g.finish(3, 110));
    // A programmatic slider animation during contact cannot confirm the value.
    g.begin(4, 0, 90);
    g.move(20, 8);
    g.progress(100, false);
    g.release();
    g.stop();
    check(!g.finish(4, 100));
    // Fold/teardown may stop tracking before the finger lifts.
    g.begin(5, 0, 0);
    g.progress(1, true);
    g.move(100, 8);
    g.stop();
    g.release();
    check(!g.finish(5, 1));
  }
}
