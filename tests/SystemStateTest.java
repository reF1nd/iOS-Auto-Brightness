// SPDX-License-Identifier: GPL-3.0-only
// SystemState against stand-ins with the fields and methods of HyperOS's
// DisplayPowerControllerImpl, AutomaticBrightnessControllerImpl and
// SceneDetector (miui-services.jar).
package iab;

public final class SystemStateTest {
  private static int failures;

  private static void check(boolean ok, String what) {
    if (!ok) {
      ++failures;
      System.err.println("FAIL: " + what);
    }
  }

  @SuppressWarnings("unused")
  static class Detector {
    boolean reflective;

    private boolean isReflectiveScene() {
      return reflective;
    }
  }

  @SuppressWarnings("unused")
  static class Automatic {
    boolean torch;
    boolean valid = true;
    boolean driving;
    boolean rear;
    private Detector mSceneDetector = new Detector();

    private boolean getIsTorchOpen() {
      return torch;
    }

    private boolean checkAssistSensorValid() {
      return valid;
    }

    private boolean getDrivingStatus() {
      return driving;
    }

    private boolean isRearScreenDevice() {
      return rear;
    }
  }

  @SuppressWarnings("unused")
  static class Impl {
    private boolean mThermalBrightnessControlAvailable = true;
    private float mThermalMaxBrightness = Float.NaN;
    private float mBatteryThermalLimitBrightness = 1.0f;
    private Automatic mAutomaticBrightnessControllerImpl = new Automatic();
  }

  public static void main(String[] args) throws Exception {
    Impl impl = new Impl();
    check(SystemState.limit(impl) == 1.0f, "no limit before the thermal controller reports");
    impl.mThermalMaxBrightness = -1.0f;
    check(SystemState.limit(impl) == 1.0f, "-1 is unset");
    impl.mThermalMaxBrightness = 0.37485194f;
    check(SystemState.limit(impl) == 0.37485194f, "thermal maximum");
    impl.mBatteryThermalLimitBrightness = 0.3f;
    check(SystemState.limit(impl) == 0.3f, "the lower battery thermal limit");
    impl.mThermalBrightnessControlAvailable = false;
    check(SystemState.limit(impl) == 0.3f, "thermal maximum ignored without thermal control");

    Automatic automatic = impl.mAutomaticBrightnessControllerImpl;
    check(SystemState.scene(impl) == 0, "clear scene");
    automatic.torch = true;
    check(SystemState.scene(impl) == SystemState.ASSIST_BLOCKED, "torch");
    automatic.torch = false;
    automatic.valid = false;
    check(SystemState.scene(impl) == SystemState.ASSIST_BLOCKED, "within 1.8 s of the torch");
    automatic.valid = true;
    automatic.driving = true;
    automatic.mSceneDetector.reflective = true;
    check(SystemState.scene(impl) == SystemState.NIGHT_DRIVING, "reflective only counts on rear-screen devices");
    automatic.rear = true;
    check(SystemState.scene(impl) == (SystemState.NIGHT_DRIVING | SystemState.REFLECTIVE), "driving and reflective");
    impl.mAutomaticBrightnessControllerImpl = null;
    check(SystemState.scene(impl) == 0, "no automatic brightness controller yet");

    try {
      SystemState.limit(new Object());
      check(false, "missing fields must throw");
    } catch (NoSuchFieldException e) {
      // The reporter then stops sending the limit.
    }

    if (failures > 0) {
      System.err.println("system_state_tests: " + failures + " failures");
      System.exit(1);
    }
    System.out.println("system_state_tests: all passed");
  }
}
