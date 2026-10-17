// SPDX-License-Identifier: GPL-3.0-only
import org.ref1nd.brightness.ProtectionPolicy;
import org.ref1nd.brightness.SensorChoice;
import org.ref1nd.brightness.OutputProbeGuard;
public final class ProtectionPolicyTest {
    static void check(boolean b,String message){if(!b)throw new AssertionError(message);}
    public static void main(String[] args) throws Exception {
        check(ProtectionPolicy.capReason(31,false,-1)==0,"Android 12 absent capability");
        check(ProtectionPolicy.capReason(32,true,1)==1,"backported reason wins");
        check(ProtectionPolicy.capReason(33,false,-1)==-1,"newer unexpected absence fails closed");
        var p=new ProtectionPolicy();
        check(p.update(0,0,0).isEmpty(),"initial clear");
        check(p.update(1,1,0).equals("system_thermal"),"even light thermal status yields");
        check(p.update(2,0,0).equals("protection_cooldown"),"no immediate reassert");
        check(p.update(10001,0,0).equals("protection_cooldown"),"full cooldown");
        check(p.update(10002,0,0).isEmpty(),"resume only when consistently clear");
        check(p.update(11000,0,1).equals("system_brightness_limit"),"thermal cap without global heat status");
        check(p.update(12000,0,2).equals("system_brightness_limit"),"power cap respected too");
        check(p.update(13000,0,99).equals("system_brightness_limit"),"future OEM reasons yield");
        check(p.update(14000,-1,0).equals("protection_unknown"),"unknown heat signal");
        check(p.update(15000,0,-1).equals("protection_unknown"),"unknown cap reason");
        String main="Display Power Controller:\n  mDisplayId=0\n  mBrightnessReason=automatic\n  mTemporaryScreenBrightness:NaN\n  mCachedBrightnessInfo.brightnessMaxReason =0\n";
        check(ProtectionPolicy.clearedTemporary(main),"confirmed lost temporary request");
        check(ProtectionPolicy.clearedTemporary(main.replace("Reason=automatic","Reason=automatic [ bcbc ]")),"BCBC alone does not transfer ownership");
        check(ProtectionPolicy.clearedTemporary("Logical Display:\n  mDisplayId=0\n"+main),"preamble is not a second power controller");
        for(String reason:new String[]{"automatic [ thermal ]","temporary","override","doze","automatic [ low_pwr ]","unknown","automatic [ dim ]","automatic [ bcbc dim ]","automatic [ bcbc thermal ]","automatic [ bcbc unknown ]","temporary [ bcbc ]"})
            check(!ProtectionPolicy.clearedTemporary(main.replace("Reason=automatic","Reason="+reason)),"cannot recover protected or unknown output");
        check(!ProtectionPolicy.clearedTemporary(main.replace(":NaN",":0.1")),"active override must not be fought");
        check(!ProtectionPolicy.clearedTemporary(main.replace("Reason =0","Reason =1")),"cannot recover thermal cap");
        check(!ProtectionPolicy.clearedTemporary(main.replace("mDisplayId=0","mDisplayId=1")),"rear display cannot authorize write");
        check(!ProtectionPolicy.clearedTemporary(main+main),"ambiguous controller rejected");
        for(String reason:new String[]{"automatic","automatic [ dim ]","automatic [ bcbc dim ]","temporary"})
            check(ProtectionPolicy.referenceOutput(main.replace("Reason=automatic","Reason="+reason)),"reference owns dim and slider output");
        for(String reason:new String[]{"doze","override","automatic [ thermal ]","automatic [ unknown ]","automatic [ bcbc low_pwr ]"})
            check(!ProtectionPolicy.referenceOutput(main.replace("Reason=automatic","Reason="+reason)),"reference retains explicit protection boundaries");
        check(!ProtectionPolicy.referenceOutput(main.replace("Reason =0","Reason =1"))&&!ProtectionPolicy.referenceOutput(main+main),"cap and ambiguous reference controller rejected");
        for(String reason:new String[]{"automatic","automatic [ dim ]","automatic [ bcbc dim ]","temporary"})
            check(ProtectionPolicy.referenceOutputResult(main.replace("Reason=automatic","Reason="+reason))==OutputProbeGuard.Result.SAFE,"known unprotected reference output");
        for(String reason:new String[]{"doze","override","automatic [ thermal ]","automatic [ hdr ]","automatic [ bcbc low_pwr ]"})
            check(ProtectionPolicy.referenceOutputResult(main.replace("Reason=automatic","Reason="+reason))==OutputProbeGuard.Result.UNSAFE,"positive protection evidence is explicit unsafe");
        for(String dump:new String[]{null,"",main+main,main.replace("mDisplayId=0","mDisplayId=1"),main.replace("Reason=automatic","Reason=future_reason"),main.replace("Reason=automatic","Reason=automatic [ vendor_future ]"),main.replace("brightnessMaxReason =0","brightnessMaxReason =-1"),main.replace("brightnessMaxReason =0","brightnessMaxReason =?"),main.replace("mCachedBrightnessInfo.brightnessMaxReason =0\n","")})
            check(ProtectionPolicy.referenceOutputResult(dump)==OutputProbeGuard.Result.UNKNOWN,"missing or unknown information must not fabricate an unsafe output event");
        check(ProtectionPolicy.referenceOutputResult(main.replace("Reason =0","Reason =1"))==OutputProbeGuard.Result.UNSAFE,"explicit cap reason remains unsafe");
        check(ProtectionPolicy.referenceLearningOutput(main)&&ProtectionPolicy.referenceLearningOutput(main.replace("Reason=automatic","Reason=automatic [ bcbc ]")),"ordinary automatic output permits settled learning");
        for(String reason:new String[]{"automatic [ dim ]","automatic [ bcbc dim ]","temporary","temporary [ bcbc ]","override","automatic [ thermal ]"})
            check(!ProtectionPolicy.referenceLearningOutput(main.replace("Reason=automatic","Reason="+reason)),"DIM, active slider and protection are not a settled learning baseline");
        check(!ProtectionPolicy.referenceLearningOutput(main.replace(":NaN",":0.1")),"pending temporary slider value must finish before learning");
        String observationManual=main.replace("Reason=automatic","Reason=manual");
        String observationOld=observationManual.replace("  mCachedBrightnessInfo.brightnessMaxReason =0\n","");
        check(ProtectionPolicy.referenceObservationLearningOutput(main)&&ProtectionPolicy.referenceObservationLearningOutput(observationManual),"observation-only learning accepts ordinary automatic or manual emulator output");
        check(ProtectionPolicy.referenceObservationLearningOutput(observationOld),"observation-only older adapter may omit cap reason");
        check(ProtectionPolicy.referenceObservationLearningOutput(observationOld.replace("Reason=manual","Reason=manual [ bcbc ]")),"unprotected vendor compensation may be observed");
        for(String reason:new String[]{"automatic [ dim ]","manual [ dim ]","manual [ bcbc dim ]","manual [ thermal ]","manual [ low_pwr ]","manual [ hdr ]","manual [ future_modifier ]","temporary","override","doze","future_reason"})
            check(!ProtectionPolicy.referenceObservationLearningOutput(main.replace("Reason=automatic","Reason="+reason)),"observation never learns protected, temporary, dim or unknown output");
        for(String dump:new String[]{null,"",main+main,observationOld.replace(":NaN",":0.1"),observationOld.replace("  mTemporaryScreenBrightness:NaN\n",""),main.replace("brightnessMaxReason =0","brightnessMaxReason =1"),main.replace("brightnessMaxReason =0","brightnessMaxReason =-1"),main.replace("brightnessMaxReason =0","brightnessMaxReason =unknown"),main.replace("brightnessMaxReason =0","brightnessMaxReason :0"),main+"  mCachedBrightnessInfo.brightnessMaxReason =0\n",main+"  mBrightnessReason=manual\n",main+"  mTemporaryScreenBrightness:NaN\n"})
            check(!ProtectionPolicy.referenceObservationLearningOutput(dump),"missing temporary evidence, ambiguous fields and present unknown caps are rejected");
        check(!ProtectionPolicy.referenceOutput(observationManual)&&!ProtectionPolicy.referenceLearningOutput(observationManual),"observation allowance never broadens active manual-output permissions");
        check(!ProtectionPolicy.referenceOutput(observationOld)&&!ProtectionPolicy.referenceLearningOutput(observationOld),"observation missing-cap allowance never authorizes active control or learning");
        check(SensorChoice.luxSensor(5,"Ambient Light","android.sensor.light"),"standard calibrated lux");
        check(SensorChoice.luxSensor(33171055,"Rear ALS","vendor.sensor.back_lux"),"vendor declared lux");
        check(!SensorChoice.luxSensor(33171111,"Ambient Light Raw Data","vendor.sensor.ambientlight.raw"),"raw channels are not lux");
        check(!SensorChoice.luxSensor(33172112,"Optimization back light fov Sensor",""),"field of view is not lux");
        var s=new SensorChoice("front=Exact Light\nback=none\nsysfs=/sys/bus/iio/devices/iio:device0/in_illuminance_input\n");
        check(new SensorChoice(s.encode()).front.equals("Exact Light"),"selection round trip");
        for(String bad:new String[]{"front=a\nfront=b", "unknown=a", "sysfs=/data/secret/lux", "sysfs=/sys/../data/lux","sysfs=/sys/raw_adc","front=a\u0001b"}) {
            try{new SensorChoice(bad);throw new AssertionError("accepted "+bad);}catch(IllegalArgumentException expected){}
        }
        check(!SensorChoice.scope("rear:a").equals(SensorChoice.scope("front:a")),"source learning separation");
        System.out.println("Protection priority, cooldown, conservative recovery and explicit sensor selection passed");
    }
}
