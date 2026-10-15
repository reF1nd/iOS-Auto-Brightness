// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

import java.util.regex.*;

/** System protection always wins. No temperature spoofing or thermal service writes. */
public final class ProtectionPolicy {
    private long clearAt=-1;
    private boolean held=false;
    public static int capReason(int api,boolean present,int value) {
        // Android 12/12L precede BrightnessInfo.brightnessMaxReason. This is an
        // absent capability, not a successful probe; recovery stays disabled.
        return !present&&api<=32?0:value;
    }
    public String update(long now,int thermalStatus,int maxReason) {
        String reason=thermalStatus>0?"system_thermal":maxReason>0?"system_brightness_limit":
            thermalStatus<0||maxReason<0?"protection_unknown":"";
        if(!reason.isEmpty()){held=true;clearAt=-1;return reason;}
        if(held) {
            if(clearAt<0)clearAt=now;
            if(now-clearAt<10000)return "protection_cooldown";
            held=false;
        }
        return "";
    }
    // Recovery requires positive evidence that only our temporary request was cleared.
    // BCBC is a vendor brightness adjustment, not an ownership transfer. Unknown
    // modifiers, dimming, HDR and other display sections still fail closed.
    public static boolean clearedTemporary(String dump) {
        String[] sections=dump.split("(?m)^Display Power Controller:\\s*$");
        String main=null;
        for(int i=1;i<sections.length;i++) {
            String s=sections[i];
            if(Pattern.compile("(?m)^\\s*mDisplayId=0\\s*$").matcher(s).find()) {
            if(main!=null)return false;main=s;
            }
        }
        if(main==null)return false;
        return Pattern.compile("(?m)^\\s*mBrightnessReason=automatic(?:[ \\t]+\\[[ \\t]*bcbc[ \\t]*\\])?[ \\t]*$").matcher(main).find()
            && Pattern.compile("(?m)^\\s*mTemporaryScreenBrightness[=:]\\s*NaN\\s*$").matcher(main).find()
            && Pattern.compile("(?m)^\\s*mCachedBrightnessInfo.brightnessMaxReason\\s*=0\\s*$").matcher(main).find();
    }
    /** v6.8 owns automatic brightness through timeout DIM and slider requests. */
    public static boolean referenceOutput(String dump) {
        String[] sections=dump.split("(?m)^Display Power Controller:\\s*$");
        String main=null;
        for(int i=1;i<sections.length;i++)if(Pattern.compile("(?m)^\\s*mDisplayId=0\\s*$").matcher(sections[i]).find()) {
            if(main!=null)return false;main=sections[i];
        }
        if(main==null)return false;
        Matcher reason=Pattern.compile("(?m)^\\s*mBrightnessReason=(automatic|temporary)(?:[ \\t]+\\[([^\\]]*)\\])?[ \\t]*$").matcher(main);
        if(!reason.find())return false;
        String modifiers=reason.group(2);
        if(modifiers!=null)for(String modifier:modifiers.trim().split("\\s+"))
            if(!modifier.isEmpty()&&!modifier.equals("dim")&&!modifier.equals("bcbc"))return false;
        return Pattern.compile("(?m)^\\s*mCachedBrightnessInfo.brightnessMaxReason\\s*=0\\s*$").matcher(main).find();
    }

}
