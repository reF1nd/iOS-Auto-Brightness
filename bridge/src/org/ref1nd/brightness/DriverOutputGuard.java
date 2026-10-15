// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

/** Learning confirmation only. DirectBacklight still rejects every mismatched write. */
public final class DriverOutputGuard {
    private long sampledAt=-1, stableAt=-1, requested=-1;
    private float framework=Float.NaN;
    private boolean matched=false, ready=false;
    public String status="unverified", protection="";

    public void reset() {
        sampledAt=stableAt=requested=-1;framework=Float.NaN;
        matched=ready=false;status="unverified";protection="";
    }
    public void sample(long now,long raw,long actual,long maximum,float value) {
        ready=false;protection="";
        if(maximum<2||raw<0||raw>maximum||actual<0||actual>maximum||!Float.isFinite(value)||value<0||value>1) {
            reset();status=protection="backlight_readback_unavailable";return;
        }
        boolean match=Math.abs(raw-actual)<=1;
        // Two non-atomic reads can straddle a ramp. Require consecutive samples
        // with a stable request/framework baseline before judging that mismatch.
        if(sampledAt<0||now<sampledAt||now-sampledAt>1000||raw!=requested
                ||!InteractionGuard.same(value,framework)||match!=matched)stableAt=now;
        sampledAt=now;requested=raw;framework=value;matched=match;
        if(now-stableAt<500){status=match?"verifying_driver_output":"driver_output_transition";return;}
        if(match){ready=true;status="driver_output_verified";}
        else status=protection="driver_output_mismatch";
    }
    public boolean learningReady(long now) {
        return ready&&now>=sampledAt&&now-sampledAt<=1000;
    }
}
