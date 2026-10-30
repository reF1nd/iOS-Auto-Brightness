// SPDX-License-Identifier: GPL-3.0-only
package iab;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/**
 * The anchors saved in learn.conf, and writing them into a HyperOS
 * RefactorNitController (miui-services.jar) by its field names. Plain Java so
 * the host tests can run it against a stand-in controller.
 */
final class AnchorState {
  static final class Anchor {
    final float lux;
    final float nit;
    final int order;
    final boolean user;

    Anchor(float lux, float nit, int order, boolean user) {
      this.lux = lux;
      this.nit = nit;
      this.order = order;
      this.user = user;
    }
  }

  float[] defaults;
  int order = -1;
  float userLux = -1.0f;
  float userLogical = -1.0f;
  boolean good;
  final List<Anchor> anchors = new ArrayList<>();
  final List<Anchor> goodAnchors = new ArrayList<>();

  /** Parses learn.conf version 2 as written by NitController::serialize; null when unusable. */
  static AnchorState parse(String text) {
    AnchorState state = new AnchorState();
    boolean version = false;
    try {
      for (String line : text.split("\n")) {
        int split = line.indexOf('=');
        if (split <= 0) continue;
        String key = line.substring(0, split);
        String[] values = line.substring(split + 1).trim().split(" +");
        switch (key) {
          case "version":
            version = values.length == 1 && values[0].equals("2");
            break;
          case "defaults":
            state.defaults = new float[values.length];
            for (int i = 0; i < values.length; ++i) state.defaults[i] = Float.parseFloat(values[i]);
            break;
          case "order":
            state.order = Integer.parseInt(values[0]);
            break;
          case "user":
            if (values.length != 2) return null;
            state.userLux = Float.parseFloat(values[0]);
            state.userLogical = Float.parseFloat(values[1]);
            break;
          case "good_curve":
            state.good = values[0].equals("1");
            break;
          case "anchor":
          case "good":
            if (values.length != 4) return null;
            Anchor anchor = new Anchor(Float.parseFloat(values[0]), Float.parseFloat(values[1]),
                Integer.parseInt(values[2]), values[3].equals("1"));
            (key.equals("anchor") ? state.anchors : state.goodAnchors).add(anchor);
            break;
          default:
            break;
        }
      }
    } catch (NumberFormatException e) {
      return null;
    }
    if (!version || state.defaults == null || state.order < 0) return null;
    if (!ordered(state.anchors) || (state.good && !ordered(state.goodAnchors))) return null;
    return state;
  }

  private static boolean ordered(List<Anchor> anchors) {
    if (anchors.size() < 4) return false;
    for (int i = 0; i < anchors.size(); ++i) {
      Anchor anchor = anchors.get(i);
      if (!(anchor.lux >= 0.0f && Float.isFinite(anchor.lux) && anchor.nit > 0.0f && Float.isFinite(anchor.nit))) {
        return false;
      }
      if (anchor.order < 0 || (i > 0 && !(anchor.lux > anchors.get(i - 1).lux))) return false;
    }
    return true;
  }

  int userAnchors() {
    int count = 0;
    for (Anchor anchor : anchors) {
      if (anchor.user) ++count;
    }
    return count;
  }

  /** Dragged anchors the controller holds now. */
  static int userAnchors(Object controller) throws ReflectiveOperationException {
    boolean[] user = (boolean[]) field(controller, "mAnchorIsUserDrag").get(controller);
    int count = field(controller, "mAnchorCount").getInt(controller), dragged = 0;
    for (int i = 0; i < count && i < user.length; ++i) {
      if (user[i]) ++dragged;
    }
    return dragged;
  }

  /**
   * resetDefaultSpline, then the saved anchors. Every field is looked up and
   * checked before anything is written, so a mismatch leaves the controller
   * untouched. Returns why nothing was written, or null after writing.
   */
  String apply(Object controller) throws ReflectiveOperationException {
    float[] system = (float[]) field(controller, "mDefaultLogicalCurve").get(controller);
    if (system == null || system.length != defaults.length) return "the system's default curve has another shape";
    for (int i = 0; i < system.length; ++i) {
      if (Math.abs(system[i] - defaults[i]) > 1e-3f) return "learn.conf was made for another default curve";
    }
    Field max = field(controller, "MAX_ANCHORS");
    int limit = max.getInt(null);
    Target main = new Target(controller, "mAnchor");
    Target goodTarget = new Target(controller, "mGoodAnchor");
    if (anchors.size() > Math.min(limit, main.capacity())) return "too many anchors for the system";
    if (good && goodAnchors.size() > Math.min(limit, goodTarget.capacity())) return "too many anchors for the system";
    Field order = field(controller, "mOrderCounter");
    Field haveGood = field(controller, "mIsHaveGoodCurve");
    Field lux = field(controller, "mUserLux");
    Field logical = field(controller, "mUserLogicalNit");
    Method reset = method(controller, "resetDefaultSpline");

    reset.invoke(controller);
    main.write(anchors);
    if (good) goodTarget.write(goodAnchors);
    haveGood.setBoolean(controller, good);
    order.setInt(controller, this.order);
    lux.setFloat(controller, userLux);
    logical.setFloat(controller, userLogical);
    return null;
  }

  /** One set of anchor arrays: mAnchor* or mGoodAnchor*. */
  private static final class Target {
    final Object controller;
    final float[] lux;
    final float[] nit;
    final int[] order;
    final boolean[] user;
    final Field count;

    Target(Object controller, String prefix) throws ReflectiveOperationException {
      this.controller = controller;
      lux = (float[]) field(controller, prefix + "Lux").get(controller);
      nit = (float[]) field(controller, prefix + "Nit").get(controller);
      order = (int[]) field(controller, prefix + "Order").get(controller);
      user = (boolean[]) field(controller, prefix + "IsUserDrag").get(controller);
      count = field(controller, prefix + "Count");
      if (lux == null || nit == null || order == null || user == null) throw new NoSuchFieldException(prefix);
    }

    int capacity() {
      return Math.min(Math.min(lux.length, nit.length), Math.min(order.length, user.length));
    }

    void write(List<Anchor> anchors) throws IllegalAccessException {
      for (int i = 0; i < capacity(); ++i) {
        Anchor anchor = i < anchors.size() ? anchors.get(i) : null;
        lux[i] = anchor != null ? anchor.lux : 0.0f;
        nit[i] = anchor != null ? anchor.nit : 0.0f;
        order[i] = anchor != null ? anchor.order : 0;
        user[i] = anchor != null && anchor.user;
      }
      count.setInt(controller, anchors.size());
    }
  }

  static Field field(Object object, String name) throws NoSuchFieldException {
    for (Class<?> type = object.getClass(); type != null; type = type.getSuperclass()) {
      try {
        Field field = type.getDeclaredField(name);
        field.setAccessible(true);
        return field;
      } catch (NoSuchFieldException e) {
        // Try the superclass.
      }
    }
    throw new NoSuchFieldException(name);
  }

  static Method method(Object object, String name) throws NoSuchMethodException {
    for (Class<?> type = object.getClass(); type != null; type = type.getSuperclass()) {
      try {
        Method method = type.getDeclaredMethod(name);
        method.setAccessible(true);
        return method;
      } catch (NoSuchMethodException e) {
        // Try the superclass.
      }
    }
    throw new NoSuchMethodException(name);
  }
}
