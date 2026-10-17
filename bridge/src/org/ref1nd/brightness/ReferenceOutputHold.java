// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

/** Pauses reference writes without releasing backlight access or learning baselines. */
public final class ReferenceOutputHold {
    private boolean dragging=false, dimmed=false, awaitingSample=false, ordinary=false;
    private long observedAt=-1, resumedAt=-1;

    /** The bridge rejects old probe generations before calling observe. */
    public void reset() {
        dragging=dimmed=awaitingSample=ordinary=false;observedAt=resumedAt=-1;
    }
    public boolean dragging(){return dragging;}
    public boolean awaitingSample(){return awaitingSample;}
    public long resumedAt(){return resumedAt;}
    /** Only an active system request blocks the core frame itself. */
    public boolean holdFrames(){return dragging||dimmed;}
    /** After a drag, preserve the visible value until the settled sample is handled. */
    public boolean holdWrites(){return holdFrames()||awaitingSample;}
    public boolean permitFrameForLearning(){return awaitingSample&&!holdFrames()&&ordinary;}
    public String reason() {
        if(dragging)return "reference_slider_active";
        if(dimmed)return "reference_dim_active";
        if(awaitingSample)return ordinary?"reference_slider_settling":"reference_slider_probe_wait";
        return "idle";
    }

    /**
     * All flags are positive evidence from one accepted main-display probe.
     * Three false flags mean unknown, not that a previous request has ended.
     */
    public boolean observe(long now,boolean sliderActive,boolean dimActive,boolean ordinaryLearningSafe) {
        if(now<0)throw new IllegalArgumentException("negative observation time");
        if(now<observedAt)return false;
        observedAt=now;
        ordinary=ordinaryLearningSafe&&!sliderActive&&!dimActive;
        if(sliderActive||dimActive) {
            dragging=sliderActive;dimmed=dimActive;resumedAt=-1;
            if(sliderActive)awaitingSample=true;
        } else if(ordinary) {
            dragging=dimmed=false;
            if(awaitingSample&&resumedAt<0)resumedAt=now;
        }
        return true;
    }

    /**
     * The existing sampler owns its eight-second cadence and 1500 ms stability
     * check. Call after processing its SAMPLE (also a below-threshold sample),
     * or explicitly when learning is disabled/unsupported. Active requests stay
     * paused; this never changes permissions, writer ownership or core state.
     */
    public void sampleFinished(){awaitingSample=false;resumedAt=-1;}
}
