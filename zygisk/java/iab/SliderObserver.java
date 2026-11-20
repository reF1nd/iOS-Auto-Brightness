// SPDX-License-Identifier: GPL-3.0-only
package iab;

import android.content.Intent;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.Display;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.view.inspector.WindowInspector;
import android.widget.SeekBar;
import java.util.Collections;
import java.util.Set;
import java.util.WeakHashMap;

/** Installed only in Settings and SystemUI. Keep the OEM listeners and their return values. */
public final class SliderObserver implements SeekBar.OnSeekBarChangeListener,
    View.OnTouchListener, View.OnLongClickListener {
  static final String ACTION = "iab.intent.action.BRIGHTNESS_SLIDER";
  private static final String MAIN = "miui.systemui.controlcenter.panel.main.brightness.BrightnessSliderController$seekBarListener$1";
  private static final String PANEL = "miui.systemui.controlcenter.panel.secondary.brightness.BrightnessPanelSliderDelegate$seekBarListener$1";
  private static final String SETTINGS = "com.android.settings.display.BrightnessSeekBarPreference$1";
  private final SeekBar bar;
  private final SeekBar.OnSeekBarChangeListener progress;
  private final View.OnTouchListener touch;
  private final View.OnLongClickListener longClick;
  private final boolean vertical;
  private final SliderGesture gesture = new SliderGesture();

  private SliderObserver(SeekBar bar, SeekBar.OnSeekBarChangeListener progress, Object info, boolean vertical)
      throws ReflectiveOperationException {
    this.bar = bar;
    this.progress = progress;
    this.vertical = vertical;
    View.OnTouchListener oldTouch = (View.OnTouchListener) get(info, "mOnTouchListener");
    touch = oldTouch instanceof SliderObserver ? ((SliderObserver) oldTouch).touch : oldTouch;
    View.OnLongClickListener oldLong = (View.OnLongClickListener) get(info, "mOnLongClickListener");
    longClick = oldLong instanceof SliderObserver ? ((SliderObserver) oldLong).longClick : oldLong;
  }

  private static Object get(Object receiver, String name) throws ReflectiveOperationException {
    if (receiver == null) return null;
    return AnchorState.field(receiver, name).get(receiver);
  }

  /** A root may appear before its recycler binds the brightness preference. */
  public static void start(String process) {
    // Zygisk specializes the app before ActivityThread prepares its main looper.
    Thread bootstrap = new Thread(() -> {
      for (int i = 0; i < 300; i++) {
        Looper looper = Looper.getMainLooper();
        if (looper != null) {
          observe(process, new Handler(looper));
          return;
        }
        SystemClock.sleep(100);
      }
    }, "iab-slider-start");
    bootstrap.setDaemon(true);
    bootstrap.start();
  }

  private static void observe(String process, Handler main) {
    boolean settings = process.equals("com.android.settings");
    main.post(new Runnable() {
      final Set<View> roots = Collections.newSetFromMap(new WeakHashMap<View, Boolean>());
      final Set<SeekBar> candidates = Collections.newSetFromMap(new WeakHashMap<SeekBar, Boolean>());
      @Override public void run() {
        for (View root : WindowInspector.getGlobalWindowViews()) {
          if (root.getHandler() != null && root.getHandler().getLooper() == main.getLooper() && roots.add(root)) {
            root.getViewTreeObserver().addOnGlobalLayoutListener(() -> scan(root, settings, candidates));
            scan(root, settings, candidates);
          }
        }
        // The OEM can replace a listener when opening the shade without another layout.
        for (SeekBar bar : candidates) if (bar.isShown()) attach(bar, settings);
        main.postDelayed(this, 500);
      }
    });
  }

  private static void scan(View view, boolean settings, Set<SeekBar> candidates) {
    if (view.getVisibility() != View.VISIBLE) return;
    if (view instanceof ViewGroup) {
      ViewGroup group = (ViewGroup) view;
      for (int i = 0; i < group.getChildCount(); i++) scan(group.getChildAt(i), settings, candidates);
    } else if (view instanceof SeekBar) {
      String type = view.getClass().getName();
      if (settings ? !type.equals("miuix.androidbasewidget.widget.SeekBar") :
          !type.equals("miui.systemui.controlcenter.widget.VerticalSeekBar")) return;
      SeekBar bar = (SeekBar) view;
      candidates.add(bar);
      attach(bar, settings);
    }
  }

  private boolean ownsTouchListeners() throws ReflectiveOperationException {
    Object info = get(bar, "mListenerInfo");
    return get(info, "mOnTouchListener") == this &&
        get(info, "mOnLongClickListener") == (longClick == null ? null : this);
  }

  private static void attach(SeekBar bar, boolean settings) {
    try {
      Object listener = get(bar, settings ? "mOnSeekBarChangeListener" : "onSeekBarChangeListener");
      if (listener == null) return;
      SliderObserver current = listener instanceof SliderObserver ? (SliderObserver) listener : null;
      if (current != null) listener = current.progress;
      String name = listener.getClass().getName();
      if (!(settings ? SETTINGS.equals(name) : MAIN.equals(name) || PANEL.equals(name))) return;
      if (current != null && current.ownsTouchListeners()) return;
      Object info = get(bar, "mListenerInfo");
      SliderObserver observer = new SliderObserver(bar, (SeekBar.OnSeekBarChangeListener) listener, info, !settings);
      bar.setOnSeekBarChangeListener(observer);
      bar.setOnTouchListener(observer);
      if (observer.longClick != null) bar.setOnLongClickListener(observer);
    } catch (ReflectiveOperationException | RuntimeException ignored) {
      // Different or inaccessible widget implementation. Leave the OEM listener in control.
    }
  }

  private void report(boolean commit, long down, int value) {
    try {
      Intent event = new Intent(ACTION).setPackage("android").addFlags(Intent.FLAG_RECEIVER_FOREGROUND)
          .putExtra("commit", commit).putExtra("contact", down).putExtra("pid", android.os.Process.myPid())
          .putExtra("time", SystemClock.uptimeMillis()).putExtra("value", value);
      bar.getContext().sendBroadcast(event);
    } catch (RuntimeException e) { Log.w("iOS_Bright", "Cannot report brightness gesture", e); }
  }

  @Override public boolean onTouch(View v, MotionEvent event) {
    int action = event.getActionMasked();
    float axis = vertical ? event.getRawY() : event.getRawX();
    long id = event.getDownTime();
    if (event.getPointerCount() != 1 || !event.isFromSource(InputDevice.SOURCE_TOUCHSCREEN)
        || event.getToolType(0) != MotionEvent.TOOL_TYPE_FINGER || bar.getDisplay() == null
        || bar.getDisplay().getDisplayId() != Display.DEFAULT_DISPLAY) {
      gesture.cancel();
    } else if (action == MotionEvent.ACTION_DOWN) {
      gesture.begin(id, axis, bar.getProgress());
      if (gesture.down == id) report(false, id, -1);
    } else if (gesture.down == id) {
      if (action == MotionEvent.ACTION_MOVE || action == MotionEvent.ACTION_UP) {
        gesture.move(axis, ViewConfiguration.get(v.getContext()).getScaledTouchSlop());
        if (action == MotionEvent.ACTION_UP) {
          gesture.release();
          v.post(() -> {
            int value = bar.getProgress();
            if (!gesture.finish(id, value) || !bar.isAttachedToWindow()) return;
            try {
              if (ownsTouchListeners() && get(bar, vertical ? "onSeekBarChangeListener" : "mOnSeekBarChangeListener") == this) {
                report(true, id, value);
              }
            } catch (ReflectiveOperationException | RuntimeException ignored) {
              // The panel may have rebound the slider during this contact.
            }
          });
        }
      } else if (action == MotionEvent.ACTION_CANCEL) {
        gesture.cancel();
      }
    }
    return touch != null && touch.onTouch(v, event);
  }

  @Override public boolean onLongClick(View v) {
    boolean consumed = longClick.onLongClick(v);
    if (consumed) gesture.openedMenu();
    return consumed;
  }

  @Override public void onProgressChanged(SeekBar b, int value, boolean fromUser) {
    progress.onProgressChanged(b, value, fromUser);
    gesture.progress(value, fromUser);
  }
  @Override public void onStartTrackingTouch(SeekBar b) { progress.onStartTrackingTouch(b); }
  @Override public void onStopTrackingTouch(SeekBar b) {
    progress.onStopTrackingTouch(b);
    gesture.stop();
  }
}
