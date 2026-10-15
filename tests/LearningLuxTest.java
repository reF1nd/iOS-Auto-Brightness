// SPDX-License-Identifier: GPL-3.0-only
import org.ref1nd.brightness.EventGate;
import org.ref1nd.brightness.LearningLux;

public final class LearningLuxTest {
    static final long WALL=1700000000000L;
    static void check(boolean b,String msg){if(!b)throw new AssertionError(msg);}
    public static void main(String[] args) {
        LearningLux h=new LearningLux();
        h.observe(9.3f,1000,WALL+1000);
        check(EventGate.accept(WALL+4000,WALL,WALL+5000,0,0,"main","main",
                new float[]{16.752f},new long[]{WALL+3000},50,1,false,true),"real OEM event remains required");
        check(h.atEvent(WALL+4000,5000,WALL+5000)==9.3f,"save direct coordinate, not OEM fused lux");
        check(h.stillStable(WALL+4000,9.3f,6500,WALL+6500),"stable through save delay");
        check(h.atEvent(WALL+3600000,3601000,WALL+3601000)==9.3f,"stable on-change value survives an hour");
        h.observe(12,3602000,WALL+3602000);
        check(h.atEvent(WALL+3601000,3603000,WALL+3603000)==9.3f,"late event uses historical value");
        h.observe(100,3604000,WALL+3604000);
        check(!h.stillStable(WALL+3601000,9.3f,3605000,WALL+3605000),"change before save rejected");
        check(h.atEvent(WALL+3605000,3606000,WALL+3606000)<0,"unstable direct window rejected");
        h.reset();h.observe(0,1000,WALL+1000);
        check(h.atEvent(WALL+2500,3000,WALL+3000)<0,"complete pre-event window required");
        check(h.atEvent(WALL+3500,4000,WALL+4000)==0,"zero lux is valid");
        check(h.atEvent(WALL+4500,4000,WALL+4000)<0,"future event rejected");
        check(h.atEvent(WALL+3500,20000,WALL+20000)<0,"old event rejected");
        h.observe(-1,21000,WALL+21000);h.observe(9,22000,WALL+22000);
        check(h.atEvent(WALL+21500,24000,WALL+24000)<0,"unavailable gap not bridged");
        h.reset();h.observe(9,25000,WALL+25000);
        check(h.atEvent(WALL+24000,28000,WALL+28000)<0,"previous registration rejected");
        check(h.atEvent(WALL+100000,28000,WALL+100000)<0,"wall clock change invalidates history");
        h.observe(9,29000,WALL+101000);
        check(h.atEvent(WALL+104000,33000,WALL+105000)==9,"new clock epoch can learn");
        h.observe(Float.NaN,34000,WALL+106000);
        check(h.atEvent(WALL+105000,35000,WALL+107000)<0,"invalid source clears history");
        h.reset();
        for(int i=0;i<600;i++)h.observe(9+i%2,i,WALL+i);
        check(h.atEvent(WALL+590,600,WALL+600)<0,"bounded overflow never invents missing coverage");
        h.reset();h.observe(9,10000,WALL+10000);h.observe(9,1000,WALL+1000);
        check(h.atEvent(WALL+2500,3000,WALL+3000)<0,"monotonic reset starts a new window");
        System.out.println("Direct learning coordinate, event-time alignment and lifecycle checks passed");
    }
}
