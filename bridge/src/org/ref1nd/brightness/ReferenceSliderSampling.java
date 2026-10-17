// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

/** Adjacent eight-second observations, never intermediate drag samples. */
public final class ReferenceSliderSampling {
    public enum Action { NONE, BASELINE, SAMPLE }
    private int value=-1;
    private long sampledAt=-1, stableAt=-1;
    private boolean eligible=false;
    public void reset(){value=-1;sampledAt=stableAt=-1;eligible=false;}
    public Action observe(long now,int slider,boolean allowed) {
        if(slider<0||slider>255){reset();return Action.NONE;}
        boolean changed=slider!=value;
        if(changed){value=slider;stableAt=now;}
        if(!allowed||!eligible||sampledAt<0||now<sampledAt) {
            boolean baseline=changed||eligible!=allowed||sampledAt<0;
            eligible=allowed;sampledAt=now;
            return baseline?Action.BASELINE:Action.NONE;
        }
        if(now-sampledAt<8000||now-stableAt<1500)return Action.NONE;
        sampledAt=now;return Action.SAMPLE;
    }
}
