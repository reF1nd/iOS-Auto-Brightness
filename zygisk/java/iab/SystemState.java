// SPDX-License-Identifier: GPL-3.0-only
package iab;

/**
 * What the HyperOS framework (miui-services.jar) decides for the main
 * display that the daemon cannot see from outside: its thermal brightness
 * limit and the scene checks of its dual light sensor policy. Read from the
 * DisplayPowerControllerImpl of display 0 by field and method names. Plain
 * Java so the host tests can run it against stand-ins.
 */
final class SystemState {
  // DualSensorPolicy::Scene in core/dual_sensor.hpp.
  static final int ASSIST_BLOCKED = 1;
  static final int NIGHT_DRIVING = 2;
  static final int REFLECTIVE = 4;

  private SystemState() {}

  /**
   * adjustBrightnessByThermal and adjustBrightnessByBattery: the thermal
   * maximum when thermal control is available and it is set (not -1 or NaN),
   * and the battery thermal limit; 1 when neither limits.
   */
  static float limit(Object impl) throws ReflectiveOperationException {
    float limit = 1.0f;
    if (AnchorState.field(impl, "mThermalBrightnessControlAvailable").getBoolean(impl)) {
      float thermal = AnchorState.field(impl, "mThermalMaxBrightness").getFloat(impl);
      if (thermal > 0.0f) limit = Math.min(limit, thermal);
    }
    float battery = AnchorState.field(impl, "mBatteryThermalLimitBrightness").getFloat(impl);
    if (battery > 0.0f) limit = Math.min(limit, battery);
    return limit;
  }

  /**
   * DualSensorPolicy drops rear readings when AutomaticBrightnessControllerImpl
   * reports the torch (getIsTorchOpen, checkAssistSensorValid) and during
   * SceneDetector night driving, and does not switch to the rear sensor in
   * a reflective scene of a rear-screen device.
   */
  static int scene(Object impl) throws ReflectiveOperationException {
    Object automatic = AnchorState.field(impl, "mAutomaticBrightnessControllerImpl").get(impl);
    if (automatic == null) return 0;
    int scene = 0;
    if (call(automatic, "getIsTorchOpen") || !call(automatic, "checkAssistSensorValid")) scene |= ASSIST_BLOCKED;
    if (call(automatic, "getDrivingStatus")) scene |= NIGHT_DRIVING;
    if (call(automatic, "isRearScreenDevice")) {
      Object detector = AnchorState.field(automatic, "mSceneDetector").get(automatic);
      if (detector != null && call(detector, "isReflectiveScene")) scene |= REFLECTIVE;
    }
    return scene;
  }

  private static boolean call(Object object, String name) throws ReflectiveOperationException {
    return (Boolean) AnchorState.method(object, name).invoke(object);
  }
}
