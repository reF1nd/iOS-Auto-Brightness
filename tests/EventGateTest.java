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
        // Stable darkness produces one on-change sample, not continuous zeros.
        check(EventGate.accept(20000,19000,20500,0,0,"main","main",new float[]{0},new long[]{14425},8.8f,1,false,true));
        check(EventGate.accept(20000,19000,20500,0,0,"main","main",new float[]{12},new long[]{14425},30,1,false,true));
        check(!EventGate.accept(20000,19000,20500,0,0,"main","main",new float[]{0},new long[]{4999},8.8f,1,false,true));
        // A value preceding the two-second window still contributes until it changes.
        check(!EventGate.accept(20000,19000,20500,0,0,"main","main",new float[]{0,50},new long[]{15000,19500},30,1,false,true));
        check(EventGate.accept(20000,19000,20500,0,0,"main","main",new float[]{50,0},new long[]{14000,17000},8.8f,1,false,true));
        check(!EventGate.accept(20000,19000,20500,0,0,"main","main",new float[]{0,0},new long[]{19000,18000},8.8f,1,false,true));
        check(!EventGate.accept(20000,19000,20500,0,0,"main","main",new float[]{0},new long[]{22000},8.8f,1,false,true));
        System.out.println("Framework user-event filtering passed");
    }
}
