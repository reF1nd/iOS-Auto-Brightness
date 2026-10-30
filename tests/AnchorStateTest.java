// SPDX-License-Identifier: GPL-3.0-only
// AnchorState against a stand-in with the fields of HyperOS's
// RefactorNitController (miui-services.jar).
package iab;

import java.util.Arrays;

public final class AnchorStateTest {
  private static int failures;

  private static void check(boolean ok, String what) {
    if (!ok) {
      ++failures;
      System.err.println("FAIL: " + what);
    }
  }

  @SuppressWarnings("unused")
  static class Base {
    private static final int MAX_ANCHORS = 7;
    private final float[] mDefaultLogicalCurve = {0, 2, 30, 135, 0, 600, 220, 5000, 1060};
    private final float[] mAnchorLux = new float[8];
    private final float[] mAnchorNit = new float[8];
    private final int[] mAnchorOrder = new int[8];
    private final boolean[] mAnchorIsUserDrag = new boolean[8];
    private int mAnchorCount;
    private final float[] mGoodAnchorLux = new float[8];
    private final float[] mGoodAnchorNit = new float[8];
    private final int[] mGoodAnchorOrder = new int[8];
    private final boolean[] mGoodAnchorIsUserDrag = new boolean[8];
    private int mGoodAnchorCount;
    private int mOrderCounter;
    private boolean mIsHaveGoodCurve;
    private float mUserLux = 5.0f;
    private float mUserLogicalNit = 5.0f;
    int resets;

    private void resetDefaultSpline() {
      ++resets;
      float[] lux = {0, 30, 600, 5000};
      float[] nit = {2, 135, 220, 1060};
      Arrays.fill(mAnchorLux, 0);
      Arrays.fill(mAnchorNit, 0);
      Arrays.fill(mGoodAnchorLux, 0);
      Arrays.fill(mGoodAnchorNit, 0);
      System.arraycopy(lux, 0, mAnchorLux, 0, 4);
      System.arraycopy(nit, 0, mAnchorNit, 0, 4);
      System.arraycopy(lux, 0, mGoodAnchorLux, 0, 4);
      System.arraycopy(nit, 0, mGoodAnchorNit, 0, 4);
      Arrays.fill(mAnchorOrder, 0);
      Arrays.fill(mAnchorIsUserDrag, false);
      Arrays.fill(mGoodAnchorOrder, 0);
      Arrays.fill(mGoodAnchorIsUserDrag, false);
      mAnchorCount = mGoodAnchorCount = 4;
      mOrderCounter = 0;
      mIsHaveGoodCurve = false;
      mUserLux = mUserLogicalNit = -1.0f;
    }
  }

  // Fields are found on superclasses as well.
  static final class Controller extends Base {
    Controller() {
      ((Base) this).resetDefaultSpline();
      resets = 0;
    }
  }

  // learn.conf from the device after the dark drag.
  static final String DARK = "version=2\ndefaults=0 2 30 135 0 600 220 5000 1060\norder=2\nuser=0 8.78360844\n"
      + "good_curve=0\nanchor=0 8.78360844 2 1\nanchor=30 135 0 0\nanchor=600 220 0 0\nanchor=5000 1060 0 0\n";

  static final String GOOD = "version=2\ndefaults=0 2 30 135 0 600 220 5000 1060\norder=1\nuser=500 900\n"
      + "good_curve=1\nanchor=0 2 0 0\nanchor=30 135 0 0\nanchor=500 900 1 1\nanchor=600 900 0 0\n"
      + "anchor=5000 1060 0 0\ngood=0 2 0 0\ngood=30 135 0 0\ngood=500 421.134 1 1\ngood=600 421.134 0 0\n"
      + "good=5000 1060 0 0\n";

  private static Object get(Object controller, String name) throws ReflectiveOperationException {
    return AnchorState.field(controller, name).get(controller);
  }

  public static void main(String[] args) throws Exception {
    AnchorState dark = AnchorState.parse(DARK);
    check(dark != null && dark.anchors.size() == 4 && dark.userAnchors() == 1, "parses the device file");

    Controller controller = new Controller();
    check(AnchorState.userAnchors(controller) == 0, "defaults hold no drags");
    check(dark.apply(controller) == null, "writes the dark drag");
    check(controller.resets == 1, "resets first");
    float[] lux = (float[]) get(controller, "mAnchorLux");
    float[] nit = (float[]) get(controller, "mAnchorNit");
    boolean[] user = (boolean[]) get(controller, "mAnchorIsUserDrag");
    check(lux[0] == 0.0f && nit[0] == 8.78360844f && user[0] && !user[1], "first anchor is the drag");
    check(lux[3] == 5000.0f && nit[3] == 1060.0f && lux[4] == 0.0f && nit[4] == 0.0f, "unused slots stay cleared");
    check((int) get(controller, "mAnchorCount") == 4 && (int) get(controller, "mOrderCounter") == 2, "counts");
    check((float) get(controller, "mUserLux") == 0.0f && (float) get(controller, "mUserLogicalNit") == 8.78360844f,
        "last drag");
    check(!(boolean) get(controller, "mIsHaveGoodCurve"), "no good curve");
    check(AnchorState.userAnchors(controller) == 1, "one drag held");

    AnchorState good = AnchorState.parse(GOOD);
    Controller second = new Controller();
    check(good != null && good.apply(second) == null, "writes a good curve");
    float[] goodNit = (float[]) get(second, "mGoodAnchorNit");
    check((boolean) get(second, "mIsHaveGoodCurve") && goodNit[2] == 421.134f
        && (int) get(second, "mGoodAnchorCount") == 5 && (int) get(second, "mAnchorCount") == 5, "good anchors");

    // Another ROM's default curve: nothing is touched.
    Controller other = new Controller();
    ((float[]) get(other, "mDefaultLogicalCurve"))[3] = 140.0f;
    check(dark.apply(other) != null && other.resets == 0 && (float) get(other, "mUserLux") == -1.0f,
        "refuses other defaults");
    StringBuilder many = new StringBuilder(DARK.substring(0, DARK.indexOf("anchor=")));
    for (int i = 0; i < 8; ++i) many.append("anchor=").append(i * 700).append(" ").append(2 + i).append(" 0 0\n");
    Controller full = new Controller();
    AnchorState crowded = AnchorState.parse(many.toString());
    check(crowded != null && crowded.apply(full) != null && full.resets == 0, "refuses more than MAX_ANCHORS");

    check(AnchorState.parse(DARK.replace("version=2", "version=1")) == null, "rejects other versions");
    check(AnchorState.parse(DARK.replace("anchor=30 135", "anchor=0 135")) == null, "rejects unordered anchors");
    check(AnchorState.parse(DARK.replace("8.78360844 2 1", "x 2 1")) == null, "rejects broken numbers");
    check(AnchorState.parse("user_offset=0.05\n") == null, "rejects v6.8 files");
    check(AnchorState.parse("") == null, "rejects empty files");

    if (failures > 0) {
      System.err.println("anchor_state_tests: " + failures + " failures");
      System.exit(1);
    }
    System.out.println("anchor_state_tests: all passed");
  }
}
