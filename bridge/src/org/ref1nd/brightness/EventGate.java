// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;
/** Pure policy for framework-reported user events; does not infer intent from settings. */
public final class EventGate {
    public static boolean controlIntent(long time,long since,long now,int user,int activeUser,String display,String mainDisplay) {
        return time>since&&time<=now+1000&&now-time<=3000&&user==activeUser&&mainDisplay.equals(display);
    }
    public static boolean accept(long time, long since, long now, int user, int activeUser,
            String display, String mainDisplay, float[] lux, long[] samples,
            float nits, float powerFactor, boolean reducedColors, boolean eligible) {
        return rejection(time,since,now,user,activeUser,display,mainDisplay,lux,samples,
                nits,powerFactor,reducedColors,eligible).isEmpty();
    }
    public static String rejection(long time, long since, long now, int user, int activeUser,
            String display, String mainDisplay, float[] lux, long[] samples,
            float nits, float powerFactor, boolean reducedColors, boolean eligible) {
        if (!eligible) return "inactive_or_obstructed";
        if (time <= since || time > now + 1000 || now - time > 15000) return "event_time";
        if (user != activeUser || !mainDisplay.equals(display)) return "other_user_or_display";
        if (!Float.isFinite(nits) || nits <= 0 || nits > 20000) return "brightness";
        if (!Float.isFinite(powerFactor) || Math.abs(powerFactor - 1) > .001 || reducedColors) return "color_or_power_correction";
        if (lux == null || samples == null || lux.length == 0 || lux.length != samples.length) return "missing_lux";
        int last = lux.length - 1;
        if (time - samples[last] > 15000) return "stale_lux";
        float min = Float.MAX_VALUE, max = 0;
        for (int i=0; i<lux.length; i++) {
            if (!Float.isFinite(lux[i]) || lux[i] < 0 || lux[i] > 200000) return "invalid_lux";
            if (samples[i] <= 0 || samples[i] > time + 1000 || (i>0 && samples[i]<samples[i-1])) return "sample_time";
            // On-change readings remain in force until their successor. Include
            // the value carried into the window, even if there was no new event.
            if (i==last || samples[i+1] >= time-2000) {
                min=Math.min(min,lux[i]);max=Math.max(max,lux[i]);
            }
        }
        return (max + 1) / (min + 1) < 2 ? "" : "unstable_lux";
    }
}
