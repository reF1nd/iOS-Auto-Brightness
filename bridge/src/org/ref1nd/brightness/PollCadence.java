// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

/** Slow expensive state reads while stable; events can wake the loop immediately. */
public final class PollCadence {
    private long stableSince=-1;
    private float reference=-1;
    public void reset() { stableSince=-1;reference=-1; }
    public static boolean changed(float a,float b) {
        return !LuxFallback.valid(a)||!LuxFallback.valid(b)||Math.abs(a-b)>Math.max(.5f,Math.abs(b)*.02f);
    }
    public int next(long now, float lux, boolean settled, boolean lowBattery) {
        if(!settled||changed(lux,reference)) {stableSince=now;reference=lux;return 250;}
        if(stableSince<0)stableSince=now;
        long stable=now-stableSince;
        if(stable>=20000)return 8000;
        if(stable>=10000)return lowBattery?2000:1600;
        return lowBattery?2000:800;
    }
}
