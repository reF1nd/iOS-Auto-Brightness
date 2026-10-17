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
        return referenceOutputResult(dump)==OutputProbeGuard.Result.SAFE;
    }
    private static String mainController(String dump) {
        if(dump==null)return null;
        String[] sections=dump.split("(?m)^Display Power Controller:\\s*$");
        String main=null;
        for(int i=1;i<sections.length;i++)if(Pattern.compile("(?m)^\\s*mDisplayId=0\\s*$").matcher(sections[i]).find()) {
            if(main!=null)return null;main=sections[i];
        }
        return main;
    }
    /** Missing/changed dump formats are waiting, not proof of a protection event. */
    public static OutputProbeGuard.Result referenceOutputResult(String dump) {
        String main=mainController(dump);
        if(main==null)return OutputProbeGuard.Result.UNKNOWN;
        Matcher reason=Pattern.compile("(?m)^\\s*mBrightnessReason=([a-z_]+)(?:[ \\t]+\\[([^\\]]*)\\])?[ \\t]*$").matcher(main);
        if(!reason.find())return OutputProbeGuard.Result.UNKNOWN;
        String source=reason.group(1);
        String modifiers=reason.group(2);
        if(reason.find())return OutputProbeGuard.Result.UNKNOWN;
        if(source.matches("manual|doze|doze_default|override|override_doze|boost|screen_off|off|offload|follower|automatic_off_display|screen_off_brightness_sensor"))
            return OutputProbeGuard.Result.UNSAFE;
        boolean unknown=!source.equals("automatic")&&!source.equals("temporary");
        if(modifiers!=null)for(String modifier:modifiers.trim().split("\\s+")) {
            if(modifier.matches("thermal|throttled|throt|low_pwr|hdr"))return OutputProbeGuard.Result.UNSAFE;
            if(!modifier.isEmpty()&&!modifier.equals("dim")&&!modifier.equals("bcbc"))unknown=true;
        }
        Matcher cap=Pattern.compile("(?m)^\\s*mCachedBrightnessInfo.brightnessMaxReason\\s*=\\s*(-?[0-9]+)\\s*$").matcher(main);
        if(!cap.find())return OutputProbeGuard.Result.UNKNOWN;
        int limit;
        try{limit=Integer.parseInt(cap.group(1));}catch(NumberFormatException e){return OutputProbeGuard.Result.UNKNOWN;}
        if(cap.find())return OutputProbeGuard.Result.UNKNOWN;
        if(limit>0)return OutputProbeGuard.Result.UNSAFE;
        return limit==0&&!unknown?OutputProbeGuard.Result.SAFE:OutputProbeGuard.Result.UNKNOWN;
    }
    /** Learn only after a slider request has settled back to ordinary automatic output. */
    public static boolean referenceLearningOutput(String dump) {
        if(referenceOutputResult(dump)!=OutputProbeGuard.Result.SAFE)return false;
        String main=mainController(dump);
        return Pattern.compile("(?m)^\\s*mBrightnessReason=automatic(?:[ \\t]+\\[[ \\t]*bcbc[ \\t]*\\])?[ \\t]*$").matcher(main).find()
            && Pattern.compile("(?m)^\\s*mTemporaryScreenBrightness[=:]\\s*NaN\\s*$").matcher(main).find();
    }
    /** Positive DIM evidence only; this does not release backlight ownership. */
    public static boolean referenceDimOutput(String dump) {
        if(referenceOutputResult(dump)!=OutputProbeGuard.Result.SAFE)return false;
        String main=mainController(dump);if(main==null)return false;
        Matcher id=Pattern.compile("(?m)^[ \\t]*mDisplayId[ \\t]*[=:][^\\r\\n]*$").matcher(main);
        if(!id.find()||!id.group().matches("[ \\t]*mDisplayId[ \\t]*=[ \\t]*0[ \\t]*")||id.find())return false;
        Matcher reason=Pattern.compile("(?m)^[ \\t]*mBrightnessReason[ \\t]*[=:][^\\r\\n]*$").matcher(main);
        if(!reason.find())return false;
        String line=reason.group();if(reason.find())return false;
        Matcher value=Pattern.compile("[ \\t]*mBrightnessReason[ \\t]*=[ \\t]*(?:automatic|temporary)[ \\t]+\\[([^\\]]*)\\][ \\t]*").matcher(line);
        if(!value.matches())return false;
        boolean dim=false;
        for(String modifier:value.group(1).trim().split("\\s+")) {
            if(modifier.equals("dim"))dim=true;
            else if(!modifier.isEmpty()&&!modifier.equals("bcbc"))return false;
        }
        Matcher cap=Pattern.compile("(?m)^[ \\t]*mCachedBrightnessInfo\\.brightnessMaxReason[^\\r\\n]*$").matcher(main);
        if(!cap.find()||!cap.group().matches("[ \\t]*mCachedBrightnessInfo\\.brightnessMaxReason[ \\t]*=[ \\t]*0[ \\t]*")||cap.find())return false;
        return dim;
    }
    /**
     * Read-only observation can exercise learning when an emulator reports manual
     * output despite its automatic setting. This never authorizes a backlight write;
     * the caller must also require observation mode and live unprotected auto mode.
     */
    public static boolean referenceObservationLearningOutput(String dump) {
        String main=mainController(dump);
        if(main==null)return false;
        Matcher reason=Pattern.compile("(?m)^[ \\t]*mBrightnessReason[ \\t]*=[ \\t]*([^\\r\\n]*)$").matcher(main);
        if(!reason.find())return false;
        String source=reason.group(1).trim();
        if(reason.find()||!source.matches("(automatic|manual)(?:[ \\t]+\\[[ \\t]*bcbc[ \\t]*\\])?"))return false;
        Matcher temporary=Pattern.compile("(?m)^[ \\t]*mTemporaryScreenBrightness[=:][ \\t]*([^\\r\\n]*)$").matcher(main);
        if(!temporary.find()||!temporary.group(1).trim().equals("NaN")||temporary.find())return false;
        // Older observation-only adapters may omit this field. A present but
        // unknown/malformed/limited cap must still reject the learning sample.
        Matcher cap=Pattern.compile("(?m)^[ \\t]*mCachedBrightnessInfo\\.brightnessMaxReason[^\\r\\n]*$").matcher(main);
        if(!cap.find())return true;
        String limit=cap.group();
        return !cap.find()&&limit.matches("[ \\t]*mCachedBrightnessInfo\\.brightnessMaxReason[ \\t]*=[ \\t]*0[ \\t]*");
    }

}
