// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

import java.util.ArrayDeque;

/** The controller's direct front-light coordinate at a real user event's time. */
public final class LearningLux {
    private static final long WINDOW = 2000, RETENTION = 20000;
    private static final int LIMIT = 512;
    private static final class Sample {
        final long at; final float lux;
        Sample(long at, float lux) { this.at=at; this.lux=lux; }
    }
    private final ArrayDeque<Sample> samples=new ArrayDeque<>();
    private long offset, lastNow=-1;

    public void reset() { samples.clear(); lastNow=-1; }

    private boolean clock(long now, long wall) {
        long next=wall-now;
        boolean continuous=lastNow<0 || (now>=lastNow && Math.abs(next-offset)<=1000);
        if(!continuous)samples.clear();
        lastNow=now;offset=next;
        return continuous;
    }

    public void observe(float lux, long now, long wall) {
        clock(now,wall);
        if(!Float.isFinite(lux)||lux<0||lux>200000){samples.clear();return;}
        // Delivery time matches when the controller can first use the reading.
        // A stable on-change sample remains in force until its successor.
        if(samples.isEmpty()||samples.getLast().lux!=lux)samples.addLast(new Sample(now,lux));
        while(samples.size()>1) {
            var it=samples.iterator();it.next();
            if(it.next().at>now-RETENTION)break;
            samples.removeFirst();
        }
        while(samples.size()>LIMIT)samples.removeFirst();
    }

    public float atEvent(long eventWall, long now, long wall) {
        if(!clock(now,wall))return -1;
        long at=eventWall-offset;
        if(at>now||now-at>15000)return -1;
        return stable(at-WINDOW,at);
    }

    public boolean stillStable(long eventWall, float chosen, long now, long wall) {
        if(!clock(now,wall))return false;
        long at=eventWall-offset;
        float current=stable(at-WINDOW,now);
        return at<=now&&now-at<=15000&&current>=0&&chosen>=0
                &&(Math.max(current,chosen)+1)/(Math.min(current,chosen)+1)<2;
    }

    private float stable(long from, long until) {
        if(samples.isEmpty()||samples.getFirst().at>from||until<from)return -1;
        float value=-1,min=Float.MAX_VALUE,max=0;
        for(Sample s:samples) {
            if(s.at>until)break;
            if(s.at<=from){value=s.lux;min=value;max=value;}
            else {value=s.lux;min=Math.min(min,value);max=Math.max(max,value);}
        }
        return value>=0&&(max+1)/(min+1)<2?value:-1;
    }
}
