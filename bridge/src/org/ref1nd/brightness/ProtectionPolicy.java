// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

import java.util.regex.*;

/** System protection always wins. No temperature spoofing or thermal service writes. */
public final class ProtectionPolicy {
    private long clearAt=-1;
    private boolean held=false;
    public String update(long now,int thermalStatus,int maxReason) {
        String reason=thermalStatus<0||maxReason<0?"protection_unknown":
            maxReason!=0?"system_brightness_limit":thermalStatus>0?"system_thermal":"";
        if(!reason.isEmpty()){held=true;clearAt=-1;return reason;}
        if(held) {
            if(clearAt<0)clearAt=now;
            if(now-clearAt<10000)return "protection_cooldown";
            held=false;
        }
        return "";
    }
    // Recovery requires positive evidence that only our temporary request was cleared.
    // Unknown OEM formats, modifiers, dimming, HDR and other display sections fail closed.
    public static boolean clearedTemporary(String dump) {
        String[] sections=dump.split("(?m)^Display Power Controller:\\s*$");
        String main=null;
        for(String s:sections)if(Pattern.compile("(?m)^\\s*mDisplayId=0\\s*$").matcher(s).find()) {
            if(main!=null)return false;main=s;
        }
        if(main==null)return false;
        return Pattern.compile("(?m)^\\s*mBrightnessReason=automatic\\s*$").matcher(main).find()
            && Pattern.compile("(?m)^\\s*mTemporaryScreenBrightness[=:]\\s*NaN\\s*$").matcher(main).find()
            && Pattern.compile("(?m)^\\s*mCachedBrightnessInfo.brightnessMaxReason\\s*=0\\s*$").matcher(main).find();
    }
}
