// SPDX-License-Identifier: GPL-3.0-only
import org.ref1nd.brightness.PanelProfile;
import org.ref1nd.brightness.ThermalPolicy;
import org.ref1nd.brightness.WindowPolicy;
public final class CompatibilityTest {
    static void check(boolean b,String message){if(!b)throw new AssertionError(message);}
    public static void main(String[] args) throws Exception {
        String dump="DisplayDeviceInfo{uniqueId=\"front\"} mBacklight=[0.1, 0.9], mNits=[1, 500], mInterpolationType=0mBrightness=[0, 1]\nDisplayDeviceInfo{uniqueId=\"rear\"} mNits=[1, 200], mBrightness=[0.2, 0.8]";
        var p=PanelProfile.fromDump(dump,"front");
        check(p.calibrated&&p.brightness[0]==0&&p.nits[1]==500,"uses framework brightness, not HAL backlight");
        check(p.matches(p.encode("example")),"profile serialization round trip");
        check(!p.id.equals(PanelProfile.fromDump(dump,"rear").id),"separate panel learning identity");
        check(!p.id.equals(PanelProfile.fromDump(dump.replace("500","600"),"front").id),"calibration changes isolate learning");
        try{PanelProfile.fromDump(dump.replace("mBrightness=[0, 1]","mBrightness=[1, 0]"),"front");throw new AssertionError("decreasing mapping");}catch(IllegalArgumentException expected){}
        try{PanelProfile.fromDump(dump.replace("mBrightness=[0, 1]","mBrightness=null"),"front");throw new AssertionError("must not borrow rear mapping");}catch(IllegalArgumentException expected){}
        try{PanelProfile.fromDump(dump,"unknown");throw new AssertionError("identity");}catch(IllegalArgumentException expected){}
        check(!PanelProfile.generic("front").calibrated,"generic mapping cannot teach nits");
        check(ThermalPolicy.boostSource("battery_therm")&&ThermalPolicy.boostSource("quiet_therm")&&ThermalPolicy.boostSource("back_therm"),"existing boost temperature aliases");
        check(ThermalPolicy.boostSource("soc")&&ThermalPolicy.boostSource("SoC_thermal")&&ThermalPolicy.boostSource("soc_cpu"),"reference SoC boost sources including mixed names");
        check(!ThermalPolicy.boostSource("cpu_back")&&!ThermalPolicy.boostSource("gpu")&&!ThermalPolicy.boostSource("pmic"),"unrelated temperature sources remain excluded");
        check(WindowPolicy.parse("mScreenBrightnessOverrideFromWindowManager=NaN\n")==0,"AOSP no override");
        check(WindowPolicy.parse("mScreenBrightnessOverrideFromWindowManager=0.5\n")==1,"app override");
        check(WindowPolicy.parse("mScreenBrightnessOverrideFromWindowManager={0=NaN, 1=0.8}")==0,"main display only");
        check(WindowPolicy.parse("mScreenBrightnessOverrideFromWindowManager={1=NaN}")==-1,"missing main unknown");
        check(WindowPolicy.parse("other=NaN")==-1,"unknown format fails closed");
        System.out.println("Calibration identity, coordinate separation, generic fallback, thermal and window compatibility passed");
    }
}
