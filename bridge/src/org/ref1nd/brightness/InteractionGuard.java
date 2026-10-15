// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

import java.util.ArrayDeque;
import java.util.regex.*;

/** Output ownership is not evidence for learning. Never overwrite an external drag. */
public final class InteractionGuard {
    private static final class Echo {
        final long at; final float value;
        Echo(long at,float value){this.at=at;this.value=value;}
    }
    private final ArrayDeque<Echo> echoes=new ArrayDeque<>();
    private float commanded=Float.NaN, observed=Float.NaN;
    private boolean waiting=false;
    private long epoch=0;
    public String status="idle";
    static boolean same(float a,float b){return Float.isFinite(a)&&Float.isFinite(b)&&Math.abs(a-b)<=Math.max(.000002f,Math.abs(b)*.001f);}
    public void wrote(long now,float before,float value) {
        // A queued display notification can still expose a previous requested value.
        echoes.addLast(new Echo(now,before));echoes.addLast(new Echo(now,value));
        while(echoes.size()>16)echoes.removeFirst();
        commanded=value;waiting=false;epoch++;status="idle";
    }
    public boolean ours(long now,float value) {
        while(!echoes.isEmpty()&&now-echoes.getFirst().at>750)echoes.removeFirst();
        if(same(value,commanded))return true;
        for(Echo echo:echoes)if(same(value,echo.value))return true;
        return false;
    }
    public void signal(){waiting=true;epoch++;status="waiting_for_temporary_release";}
    public boolean observe(long now,float value,boolean owned) {
        if(!Float.isFinite(value)||value<0||value>1)return false;
        boolean changed=(waiting||status.equals("released"))&&!same(value,observed);
        if((owned&&!ours(now,value))||changed) {
            observed=value;signal();return true;
        }
        observed=value;return false;
    }
    public boolean waiting(){return waiting;}
    public long epoch(){return epoch;}
    public void probe(long expected,int temporary) {
        if(!waiting||epoch!=expected)return;
        if(temporary==0){waiting=false;status="released";}
        else status=temporary==1?"external_temporary_brightness":"temporary_state_unknown";
    }
    public void reset(){echoes.clear();commanded=observed=Float.NaN;waiting=false;epoch++;status="idle";}
    /** 0 cleared, 1 active, -1 unavailable/ambiguous. Main display only. */
    public static int temporary(String dump) {
        String main=null;
        String[] sections=dump.split("(?m)^Display Power Controller:\\s*$");
        for(int i=1;i<sections.length;i++) {
            String section=sections[i];
            if(Pattern.compile("(?m)^\\s*mDisplayId=0\\s*$").matcher(section).find()) {
                if(main!=null)return -1;main=section;
            }
        }
        if(main==null)return -1;
        Matcher m=Pattern.compile("(?m)^\\s*mTemporaryScreenBrightness[=:]\\s*(\\S+)\\s*$").matcher(main);
        if(!m.find())return -1;
        String value=m.group(1);if(m.find())return -1;
        if(value.equals("NaN"))return 0;
        try{float v=Float.parseFloat(value);return Float.isFinite(v)&&v>=0&&v<=1?1:-1;}
        catch(NumberFormatException e){return -1;}
    }
}
