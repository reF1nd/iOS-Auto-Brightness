// SPDX-License-Identifier: GPL-3.0-only
package iab;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Build;
import android.os.Handler;
import android.os.SystemClock;
import android.util.Log;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.concurrent.ArrayBlockingQueue;

/** Runs on the display handler. Pair the UI commit with exactly one new main-display anchor. */
final class DragReceiver extends BroadcastReceiver {
  private final Handler handler;
  private final Object controller;
  private final Field order, automatic, lux, nit;
  private final Method sliderToNit;
  private final ArrayBlockingQueue<String> confirmed;
  private final SliderCommit pending = new SliderCommit();
  private int generation;

  private DragReceiver(Object controller, Handler handler, ArrayBlockingQueue<String> confirmed)
      throws ReflectiveOperationException {
    this.handler = handler;
    this.controller = controller;
    this.confirmed = confirmed;
    order = AnchorState.field(controller, "mOrderCounter");
    automatic = AnchorState.field(controller, "mUseAutoBrightness");
    lux = AnchorState.field(controller, "mUserLux");
    nit = AnchorState.field(controller, "mUserLogicalNit");
    sliderToNit = Class.forName("com.android.server.display.RefactorBrightnessUtil", false,
        controller.getClass().getClassLoader()).getMethod("sliderToLogicalBrightness", float.class);
  }

  static DragReceiver register(Object controller, Handler handler, ArrayBlockingQueue<String> confirmed) {
    try {
      Context context = (Context) AnchorState.field(controller, "mContext").get(controller);
      DragReceiver receiver = new DragReceiver(controller, handler, confirmed);
      IntentFilter filter = new IntentFilter(SliderObserver.ACTION);
      String permission = "android.permission.CONTROL_DISPLAY_BRIGHTNESS";
      if (Build.VERSION.SDK_INT >= 33) {
        context.registerReceiver(receiver, filter, permission, handler, Context.RECEIVER_EXPORTED);
      } else {
        context.registerReceiver(receiver, filter, permission, handler);
      }
      return receiver;
    } catch (ReflectiveOperationException | RuntimeException e) {
      Log.w("iOS_Bright", "Slider confirmation unavailable", e);
      return null;
    }
  }

  void clear() {
    generation++;
    pending.begin(-1, 0, 0, false);
    confirmed.clear();
  }

  @Override public void onReceive(Context ctx, Intent event) {
    if (!SliderObserver.ACTION.equals(event.getAction())) return;
    long now = SystemClock.uptimeMillis();
    long sent = event.getLongExtra("time", -1);
    long down = event.getLongExtra("contact", -1);
    int source = event.getIntExtra("pid", -1);
    if (sent < down || down < 0 || source <= 0 || sent > now || now - sent > SliderCommit.MAX_DELAY_MS) return;
    try {
      if (!event.getBooleanExtra("commit", false)) {
        generation++;
        pending.begin(down, source, order.getInt(controller), automatic.getBoolean(controller));
        return;
      }
      if (!pending.consume(down, source)) return;
      int value = event.getIntExtra("value", -1);
      if (value < 0 || value > 16383) return;
      float expected = (float) sliderToNit.invoke(null, value / 16383.0f);
      match(generation, expected, sent + SliderCommit.MAX_DELAY_MS);
    } catch (ReflectiveOperationException | RuntimeException e) {
      clear();
      Log.w("iOS_Bright", "Cannot match slider commit", e);
    }
  }

  private void match(int token, float expected, long deadline) {
    if (token != generation || SystemClock.uptimeMillis() > deadline) return;
    try {
      if (!automatic.getBoolean(controller)) return;
      int sequence = order.getInt(controller);
      if (pending.waiting(sequence)) {
        handler.postDelayed(() -> match(token, expected, deadline), 40);
      } else {
        float logical = nit.getFloat(controller), at = lux.getFloat(controller);
        if (pending.matches(sequence, expected, at, logical)) {
          confirmed.offer(SystemClock.elapsedRealtime() + " " + at + " " + logical);
        }
      }
    } catch (IllegalAccessException e) { Log.w("iOS_Bright", "Cannot read committed anchor", e); }
  }
}
