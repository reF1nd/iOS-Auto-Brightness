// SPDX-License-Identifier: GPL-3.0-only
import org.ref1nd.brightness.EventGate;
public final class EventGateTest {
    static void check(boolean b){if(!b)throw new AssertionError();}
    static boolean event(long stamp,int user,String display,float[] lux,float nits,boolean eligible){return EventGate.accept(stamp,10000,12000,user,0,display,"main",lux,new long[]{11000},nits,1,false,eligible);}
    public static void main(String[] args) {
        check(event(11500,0,"main",new float[]{10},100,true));
        check(!event(9000,0,"main",new float[]{10},100,true));
        check(!event(14000,0,"main",new float[]{10},100,true));
        check(!event(11500,1,"main",new float[]{10},100,true));
        check(!event(11500,0,"rear",new float[]{10},100,true));
        check(!event(11500,0,"main",new float[]{Float.NaN},100,true));
        check(!event(11500,0,"main",new float[]{10},Float.POSITIVE_INFINITY,true));
        check(!event(11500,0,"main",new float[]{10},100,false));
        check(!EventGate.accept(11500,10000,12000,0,0,"main","main",new float[]{1,50},new long[]{10900,11000},100,1,false,true));
        check(!EventGate.accept(11500,10000,12000,0,0,"main","main",new float[]{10},new long[]{11000},100,.5f,false,true));
        System.out.println("Framework user-event filtering passed");
    }
}
