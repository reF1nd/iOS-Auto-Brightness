// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

/** Opt-in heuristic. Settings deltas cannot prove user intent. */
public final class SettingsLearning {
    private String slider, adjustment;
    private long eligibleAt=-1;
    public void reset(){slider=null;adjustment=null;eligibleAt=-1;}
    public boolean observe(long now,boolean eligible,String nextSlider,String nextAdjustment) {
        if(!eligible){reset();return false;}
        if(eligibleAt<0)eligibleAt=now;
        boolean changed=now-eligibleAt>=2000&&
            ((slider!=null&&nextSlider!=null&&!slider.equals(nextSlider))||
             (adjustment!=null&&nextAdjustment!=null&&!adjustment.equals(nextAdjustment)));
        slider=nextSlider;adjustment=nextAdjustment;
        return changed;
    }
}
