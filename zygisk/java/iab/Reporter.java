// SPDX-License-Identifier: GPL-3.0-only
package iab;

import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Handler;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructPollfd;
import android.util.Log;
import android.view.Display;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

/**
 * Runs in system_server after the boot restore. Connects to the daemon's
 * abstract socket and sends the main display's state and the
 * RefactorNitController anchors whenever they change, as
 * "<type> <length>\n<payload>" (core/system_link.hpp reads them). The daemon
 * may send "reset\n" to clear the anchors. Only a root peer is served.
 */
final class Reporter {
  static final String SOCKET = "iab_brightness";
  static final int VERSION = 1;
  private static final String TAG = "iOS_Bright";
  private static final int TICK_MS = 500;
  private static final long RETRY_MIN_MS = 1000;
  private static final long RETRY_MAX_MS = 10_000;

  private final Handler handler;
  private final Object controller;
  private final Object displayController;
  private Object displays;
  private Method realDisplay;

  Reporter(Handler handler, Object controller, Object displayController) {
    this.handler = handler;
    this.controller = controller;
    this.displayController = displayController;
  }

  void run() throws InterruptedException, ReflectiveOperationException {
    Class<?> global = Class.forName("android.hardware.display.DisplayManagerGlobal");
    displays = global.getMethod("getInstance").invoke(null);
    realDisplay = global.getMethod("getRealDisplay", int.class);
    long retry = RETRY_MIN_MS;
    while (true) {
      LocalSocket socket = new LocalSocket();
      try {
        socket.connect(new LocalSocketAddress(SOCKET, LocalSocketAddress.Namespace.ABSTRACT));
        if (socket.getPeerCredentials().getUid() != 0) throw new IOException("the listener is not root");
        retry = RETRY_MIN_MS;
        serve(socket);
      } catch (IOException e) {
        // The daemon is not running yet, or went away.
      } finally {
        try {
          socket.close();
        } catch (IOException e) {
          // Already closed.
        }
      }
      Thread.sleep(retry);
      retry = Math.min(retry * 2, RETRY_MAX_MS);
    }
  }

  private void serve(LocalSocket socket) throws IOException, InterruptedException, ReflectiveOperationException {
    OutputStream out = socket.getOutputStream();
    InputStream in = socket.getInputStream();
    StructPollfd poll = new StructPollfd();
    poll.fd = socket.getFileDescriptor();
    poll.events = (short) OsConstants.POLLIN;
    send(out, "hello", Integer.toString(VERSION));
    int state = Integer.MIN_VALUE;
    String anchors = null;
    StringBuilder command = new StringBuilder();
    byte[] buffer = new byte[64];
    while (true) {
      int now = displayState();
      if (now != state) {
        state = now;
        send(out, "state", Integer.toString(now));
      }
      String snapshot = snapshot();
      if (snapshot != null && !snapshot.equals(anchors)) {
        anchors = snapshot;
        send(out, "anchors", snapshot);
      }
      poll.revents = 0;
      try {
        if (Os.poll(new StructPollfd[] {poll}, TICK_MS) == 0) continue;
      } catch (ErrnoException e) {
        if (e.errno == OsConstants.EINTR) continue;
        throw new IOException(e);
      }
      int read = in.read(buffer);
      if (read < 0) return;
      for (int i = 0; i < read; ++i) {
        if (buffer[i] != '\n') {
          if (command.length() < 32) command.append((char) buffer[i]);
          continue;
        }
        if ("reset".contentEquals(command)) reset();
        command.setLength(0);
      }
    }
  }

  private int displayState() throws ReflectiveOperationException {
    Display display = (Display) realDisplay.invoke(displays, Display.DEFAULT_DISPLAY);
    return display == null ? Display.STATE_UNKNOWN : display.getState();
  }

  /** Read on the display handler, so never halfway through a drag; null if it is busy. */
  private String snapshot() throws InterruptedException, ReflectiveOperationException {
    FutureTask<String> task = new FutureTask<>(new Callable<String>() {
      @Override
      public String call() throws ReflectiveOperationException {
        return AnchorState.snapshot(controller);
      }
    });
    if (!handler.post(task)) return null;
    try {
      return task.get(1, TimeUnit.SECONDS);
    } catch (TimeoutException e) {
      task.cancel(false);
      return null;
    } catch (ExecutionException e) {
      if (e.getCause() instanceof ReflectiveOperationException) throw (ReflectiveOperationException) e.getCause();
      throw new IllegalStateException(e.getCause());
    }
  }

  private void reset() {
    handler.post(new Runnable() {
      @Override
      public void run() {
        try {
          AnchorState.method(controller, "resetDefaultSpline").invoke(controller);
          AnchorState.method(displayController, "updateBrightness").invoke(displayController);
          Log.i(TAG, "System anchors: reset by the module");
        } catch (Throwable t) {
          Log.w(TAG, "System anchors: reset failed", t);
        }
      }
    });
  }

  private static void send(OutputStream out, String type, String payload) throws IOException {
    byte[] body = payload.getBytes(StandardCharsets.US_ASCII);
    out.write((type + " " + body.length + "\n").getBytes(StandardCharsets.US_ASCII));
    out.write(body);
    out.flush();
  }
}
