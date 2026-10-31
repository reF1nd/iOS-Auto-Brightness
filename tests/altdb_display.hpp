// SPDX-License-Identifier: GPL-3.0-only
// Test data: an excerpt of dumpsys display on Xiaomi 17 Pro Max (altdb,
// HyperOS 4), 2026-10-30. The daemon reads these values from the running
// system; the tests use this device's as an example.
#pragma once

#include "display_dump.hpp"

namespace iab::test {

inline const char kAltdbDisplayDump[] = R"(DISPLAY MANAGER (dumpsys display)
  mDisplayDeviceConfig=DisplayDeviceConfig{
    mLuxThrottlingData={}, mHbmData=HBM{minLux: 9999.0, transition: 0.5, timeWindow: 0ms}
    mAmbientHorizonLong=4000, mAmbientHorizonShort=2000
  }

Display Power Controllers: size=2

Display Power Controller:
-------------------------
  mDisplayId=0
  mLeadDisplayId=-1
  mLightSensor={Sensor name="tcs3760 Ambient Light Sensor Non-wakeup", vendor="amsAG", version=65794, type=5}

HighBrightnessModeController:
  mHbmControllerIsEnabled=true
  mHbmData=HBM{minLux: 4475.0, transition: 0.37486267, timeWindow: 0ms, timeMax: 0ms, timeMin: 0ms}

Automatic Brightness Controller Configuration:
----------------------------------------------
  mState=AUTO_BRIGHTNESS_ENABLED
  mLightSensorWarmUpTimeConfig=0
  mBrighteningLightDebounceConfig=1000
  mDarkeningLightDebounceConfig=1000
  mBrighteningLightDebounceConfigIdle=1000
  mDarkeningLightDebounceConfigIdle=1000
  mSmallBrighteningLightDebounceConfig=5000
  mResetAmbientLuxAfterWarmUpConfig=true
  mAmbientLightHorizonLong=3000
  mAmbientLightHorizonShort=1000
  mWeightingIntercept=3000

  Dual Sensor Policy State:
    mUseLightSensorFlag=1
    mAssistAmbientLuxValid=true
  Dual Sensor Policy Configuration:
    mAssistLightSensorBrighteningDebounce=1000
    mAssistLightSensorSmallBrighteningDebounce=5000
    mAssistLightSensorDarkeningDebounce=1000

  Scene Detector State:
    mMinDrivingEnableNit=5.0
  MiuiHysteresisLevels:
    mHysteresisBrightSpline=LinearSpline{[(0.0, 2.0: 1.3333334), (3.0, 6.0: 2.5100002), (4.0, 8.51: 2.5749998), (6.0, 13.66: 1.1800003), (7.0, 14.84: 0.6199999), (8.0, 15.46: 1.7599993), (9.0, 17.22: 1.8700002), (15.0, 28.44: 8.770666), (30.0, 160.0: 1.4606667), (600.0, 992.58: 1.8632076), (1677.4, 3000.0: 3.219994), (2000.0, 4038.77: 3.4), (2500.0, 5738.77: 3.6), (3000.0, 7538.77: 0.99999994), (8500.0, 13038.77)]}
    mHysteresisDarkSpline=LinearSpline{[(0.0, 0.0: 0.32), (1.0, 0.32: 0.535), (7.0, 3.53: 0.26999998), (8.0, 3.8: 0.29999995), (9.0, 4.1: 0.3499999), (10.0, 4.45: 0.37000036), (11.0, 4.82: 0.37999964), (12.0, 5.2: 0.3800001), (13.0, 5.58: 0.47000027), (14.0, 6.05: 0.8499999), (15.0, 6.9: 0.59533334), (30.0, 15.83: 0.109349124), (199.0, 34.31: 7.079998), (200.0, 41.39: 0.024687504), (360.0, 45.34: 0.71520835), (600.0, 216.99: 0.9331012), (1010.47, 600.0: 0.7301021), (3000.0, 2052.56: 0.4682), (20000.0, 10011.96)]}
    mHysteresisSmallBrightSpline=LinearSpline{[(0.0, 2.0: 1.3333334), (3.0, 6.0: 2.5100002), (4.0, 8.51: 2.5749998), (6.0, 13.66: 1.1800003), (7.0, 14.84: 0.6199999), (8.0, 15.46: 1.7599993), (9.0, 17.22: 0.9740003), (15.0, 23.064001: 5.6624), (30.0, 108.0: 1.2764001), (600.0, 835.54803: 1.5179244), (1677.4, 2470.96: 2.3319972), (2000.0, 3223.2622: 2.44), (2500.0, 4443.262: 2.5910156), (3000.0, 5738.77: 0.9971803), (8500.0, 11223.262)]}

Thermal Brightness Controller:
  mCurrentAppliedThermalConditionId=0
  mCurrentThermalMaxBrightness=600.0
mThermalMaxBrightness=0.37485194
mBasedSdrBrightness=0.008645099

BCL Level:
 mBatteryThermalLevel=9
 mBatteryThermalLimitBrightness=1.0

Refactor Nit Controller Configuration:
  mDisplayId=0
  mUserLux=0.0

Display Power Controller:
-------------------------
  mDisplayId=1
  mLightSensor={Sensor name="tcs3760_sec_back Ambient Light Back Sensor Wakeup", vendor="amsAG", version=65794, type=33171055}

Automatic Brightness Controller Configuration:
  mBrighteningLightDebounceConfig=2000
  mAmbientLightHorizonLong=6000
mThermalMaxBrightness=0.2
)";

// The light sensor configuration above, as the daemon reads it.
inline DualSensorConfig altdb_dual_sensor() {
  DualSensorConfig config;
  read_dual_sensor_config(main_display_section(kAltdbDisplayDump), config);
  return config;
}

}  // namespace iab::test
