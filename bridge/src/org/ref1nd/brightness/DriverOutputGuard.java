// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

/** Confirms coherent, stable driver output before learning or judging a clamp. */
public final class DriverOutputGuard {
    private long sampledAt=-1, stableAt=-1, requested=-1, actual=-1;
    private float framework=Float.NaN;
    private boolean matched=false, ready=false;
    public String status="unverified", protection="";

    public void reset() {
        sampledAt=stableAt=requested=actual=-1;framework=Float.NaN;
        matched=ready=false;status="unverified";protection="";
    }
    public void sample(long now,long raw,long actual,long maximum,float value) {
        sample(now,raw,actual,raw,maximum,value);
    }
    /** The request must be unchanged across the independent driver read. */
    public void sample(long now,long before,long actual,long after,long maximum,float value) {
        ready=false;protection="";
        if(maximum<2||before<0||before>maximum||after<0||after>maximum||actual<0||actual>maximum||!Float.isFinite(value)||value<0||value>1) {
            reset();status=protection="backlight_readback_unavailable";return;
        }
        if(before!=after) {
            sampledAt=now;stableAt=-1;requested=this.actual=-1;framework=value;matched=false;
            status="driver_output_transition";return;
        }
        boolean match=Math.abs(before-actual)<=1;
        // A stable request with a still-moving driver is also a transition.
        if(sampledAt<0||stableAt<0||now<sampledAt||now-sampledAt>1000||before!=requested
                ||(!match&&actual!=this.actual)||!InteractionGuard.same(value,framework)||match!=matched)stableAt=now;
        sampledAt=now;requested=before;this.actual=actual;framework=value;matched=match;
        if(now-stableAt<500){status=match?"verifying_driver_output":"driver_output_transition";return;}
        if(match){ready=true;status="driver_output_verified";}
        else status=protection="driver_output_mismatch";
    }
    public boolean learningReady(long now) {
        return ready&&now>=sampledAt&&now-sampledAt<=1000;
    }
}
