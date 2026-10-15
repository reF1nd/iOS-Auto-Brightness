// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;
/** Pure policy for framework-reported user events; does not infer intent from settings. */
public final class EventGate {
    public static boolean accept(long time, long since, long now, int user, int activeUser,
            String display, String mainDisplay, float[] lux, long[] samples,
            float nits, float powerFactor, boolean reducedColors, boolean eligible) {
        if (!eligible || time <= since || time > now + 1000 || now - time > 15000
                || user != activeUser || !mainDisplay.equals(display)
                || !Float.isFinite(nits) || nits <= 0 || nits > 20000
                || !Float.isFinite(powerFactor) || Math.abs(powerFactor - 1) > .001
                || reducedColors || lux == null || samples == null
                || lux.length == 0 || lux.length != samples.length) return false;
        int last = lux.length - 1;
        if (time - samples[last] > 5000 || samples[last] > time + 1000) return false;
        float min = Float.MAX_VALUE, max = 0;
        for (int i=0; i<lux.length; i++) {
            if (!Float.isFinite(lux[i]) || lux[i] < 0 || lux[i] > 200000) return false;
            if (time - samples[i] <= 2000) { min=Math.min(min,lux[i]); max=Math.max(max,lux[i]); }
        }
        return min != Float.MAX_VALUE && (max + 1) / (min + 1) < 2;
    }
}
