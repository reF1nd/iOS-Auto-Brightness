// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

/** One explicitly armed adjustment; normalized brightness only, never slider/gamma guesses. */
public final class ManualLearning {
    public long until=0,at=-1,wall=0;
    public float lux=-1,value=-1;
    private long stableAt=0;
    public String status="idle";
    public void cancel(){until=0;at=-1;}
    public void arm(long now){until=now+30000;at=-1;status="manual_armed_adjust_slider";}
    public void change(long now,long eventWall,float eventLux) {
        if(until<=now)return;
        wall=eventWall;lux=eventLux;at=now;value=-1;stableAt=now;
    }
    public boolean settingChanged(String before,String after) {
        return until>0&&before!=null&&after!=null&&!before.equals(after);
    }
    public boolean sample(long now,boolean eligible,float brightness) {
        if(until==0)return false;
        if(now>until||!eligible){cancel();status="manual_cancelled_state";return false;}
        if(at<0)return false;
        if(now-at>10000){cancel();status="manual_expired";return false;}
        if(!Float.isFinite(brightness)||brightness<0||brightness>1){cancel();status="manual_invalid_brightness";return false;}
        if(value<0||Math.abs(brightness-value)>Math.max(.00001f,value*.01f)){value=brightness;stableAt=now;}
        return now-stableAt>=1500&&now-at>=2000;
    }
}
