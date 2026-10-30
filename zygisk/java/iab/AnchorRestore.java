// SPDX-License-Identifier: GPL-3.0-only
package iab;

import android.os.Handler;
import android.util.Log;
import android.util.SparseArray;

/**
 * Loaded into system_server by the Zygisk library. HyperOS keeps the
 * RefactorNitController anchors in memory only, so after a reboot the system
 * would write its default curve while the daemon still follows the learned
 * one. Once the controller of the main display exists, this writes the
 * anchors kept in learn.conf back into it on the display handler thread.
 * Nothing here may throw into system_server.
 */
public final class AnchorRestore {
  private static final String TAG = "iOS_Bright";
  private static final long STEP_MS = 500;
  private static final long FIND_MS = 180_000;
  private static final long BOOT_MS = 600_000;

  private final AnchorState state;
  private Object displayController;
  private Object controller;

  private AnchorRestore(AnchorState state) {
    this.state = state;
  }

  /** Called from the Zygisk library on the system_server main thread. */
  public static void start(String learn) {
    try {
      final AnchorState state = AnchorState.parse(learn);
      if (state == null) {
        Log.w(TAG, "System anchors: learn.conf is not usable");
        return;
      }
      if (state.userAnchors() == 0) return;
      Thread thread = new Thread(new Runnable() {
        @Override
        public void run() {
          new AnchorRestore(state).run();
        }
      }, "iab-anchors");
      thread.setDaemon(true);
      thread.start();
    } catch (Throwable t) {
      Log.w(TAG, "System anchors: not started", t);
    }
  }

  private void run() {
    try {
      Handler handler = null;
      for (long waited = 0; handler == null; waited += STEP_MS) {
        if (waited >= FIND_MS) {
          Log.w(TAG, "System anchors: RefactorNitController of display 0 not found");
          return;
        }
        Thread.sleep(STEP_MS);
        handler = find();
      }
      post(handler, false);
      // Loading the brightness configuration during boot may reset the
      // anchors again; restore once more if nothing was dragged since.
      for (long waited = 0; !"1".equals(property("sys.boot_completed")); waited += 2000) {
        if (waited >= BOOT_MS) return;
        Thread.sleep(2000);
      }
      post(handler, true);
    } catch (Throwable t) {
      Log.w(TAG, "System anchors: not restored", t);
    }
  }

  /** The display handler once DisplayPowerController(0) has its RefactorNitController. */
  private Handler find() throws ReflectiveOperationException {
    Object service;
    Object power;
    try {
      Object binder = Class.forName("android.os.ServiceManager").getMethod("getService", String.class)
          .invoke(null, "display");
      if (binder == null) return null;
      service = AnchorState.field(binder, "this$0").get(binder);
      SparseArray<?> controllers = (SparseArray<?>) AnchorState.field(service, "mDisplayPowerControllers").get(service);
      power = controllers == null ? null : controllers.get(0);
    } catch (RuntimeException e) {
      return null;  // Read while DisplayManagerService was still filling it.
    }
    if (power == null) return null;
    Object impl = AnchorState.field(power, "mDisplayPowerControllerImpl").get(power);
    if (impl == null) return null;
    Object refactor = AnchorState.field(impl, "mRefactorNitController").get(impl);
    Object handler = AnchorState.field(impl, "mHandler").get(impl);
    if (refactor == null || !(handler instanceof Handler)) return null;
    displayController = power;
    controller = refactor;
    return (Handler) handler;
  }

  private void post(Handler handler, final boolean again) {
    handler.post(new Runnable() {
      @Override
      public void run() {
        try {
          if (again && AnchorState.userAnchors(controller) > 0) return;
          String refused = state.apply(controller);
          if (refused != null) {
            Log.w(TAG, "System anchors: not restored, " + refused);
            return;
          }
          Log.i(TAG, "System anchors: restored " + state.userAnchors() + " dragged anchors"
              + (again ? " after a reset during boot" : ""));
          AnchorState.method(displayController, "updateBrightness").invoke(displayController);
        } catch (Throwable t) {
          Log.w(TAG, "System anchors: not restored", t);
        }
      }
    });
  }

  private static String property(String name) throws ReflectiveOperationException {
    return (String) Class.forName("android.os.SystemProperties").getMethod("get", String.class).invoke(null, name);
  }
}
