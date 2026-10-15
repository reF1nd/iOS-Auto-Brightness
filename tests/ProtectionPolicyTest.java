// SPDX-License-Identifier: GPL-3.0-only
import org.ref1nd.brightness.ProtectionPolicy;
import org.ref1nd.brightness.SensorChoice;
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
        for(String reason:new String[]{"automatic [ thermal ]","temporary","override","doze","automatic [ low_pwr ]","unknown"})
            check(!ProtectionPolicy.clearedTemporary(main.replace("Reason=automatic","Reason="+reason)),"cannot recover protected or unknown output");
        check(!ProtectionPolicy.clearedTemporary(main.replace(":NaN",":0.1")),"active override must not be fought");
        check(!ProtectionPolicy.clearedTemporary(main.replace("Reason =0","Reason =1")),"cannot recover thermal cap");
        check(!ProtectionPolicy.clearedTemporary(main.replace("mDisplayId=0","mDisplayId=1")),"rear display cannot authorize write");
        check(!ProtectionPolicy.clearedTemporary(main+main),"ambiguous controller rejected");
        var s=new SensorChoice("front=Exact Light\nback=none\nsysfs=/sys/bus/iio/devices/iio:device0/in_illuminance_input\n");
        check(new SensorChoice(s.encode()).front.equals("Exact Light"),"selection round trip");
        for(String bad:new String[]{"front=a\nfront=b", "unknown=a", "sysfs=/data/secret/lux", "sysfs=/sys/../data/lux","sysfs=/sys/raw_adc","front=a\u0001b"}) {
            try{new SensorChoice(bad);throw new AssertionError("accepted "+bad);}catch(IllegalArgumentException expected){}
        }
        check(!SensorChoice.scope("rear:a").equals(SensorChoice.scope("front:a")),"source learning separation");
        System.out.println("Protection priority, cooldown, conservative recovery and explicit sensor selection passed");
    }
}
