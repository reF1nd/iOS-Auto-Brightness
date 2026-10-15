// SPDX-License-Identifier: GPL-3.0-only
import org.ref1nd.brightness.InteractionGuard;
import org.ref1nd.brightness.EventGate;

public final class InteractionGuardTest {
    static void check(boolean b,String message){if(!b)throw new AssertionError(message);}
    public static void main(String[] args) {
        var g=new InteractionGuard();
        g.wrote(1000,.00224533f,.0020979932f);
        check(!g.observe(1050,.00224533f,true),"queued pre-write brightness is an acknowledgement");
        check(!g.observe(1100,.0020979932f,true),"own output must not trigger a hold");
        // 21:10:57 trace: system user-set brightness 7.245 nits while module wanted 1.915.
        check(g.observe(1200,.008016604f,true)&&g.waiting(),"drag immediately relinquishes ownership");
        long first=g.epoch();
        g.probe(first,1);
        check(g.waiting(),"external temporary slot remains authoritative");
        check(!g.observe(22000,.008016604f,false)&&g.waiting(),"stationary finger beyond five seconds never expires");
        g.probe(first,-1);
        check(g.waiting(),"unknown state cannot authorize overwriting a drag");
        check(g.observe(23000,.00967706f,false),"continued movement invalidates old probes");
        g.probe(first,0);
        check(g.waiting(),"old NaN snapshot cannot end newer movement");
        g.probe(g.epoch(),0);
        check(!g.waiting(),"confirmed temporary release permits normal cooldown");
        check(g.observe(24000,.012f,false)&&g.waiting(),"new drag during cooldown suspends again");
        g.reset();
        check(!g.waiting(),"screen/user session reset ends old interaction");
        g.wrote(30000,.01f,.02f);
        check(g.observe(31000,.01f,true),"old echoes expire rather than mask later input");
        g.reset();g.wrote(40000,.02f,.03f);g.wrote(40250,.03f,.04f);
        check(!g.observe(40300,.03f,true),"recent own earlier output tolerated");
        check(!g.observe(41000,.0400001f,true),"float rounding does not manufacture user activity");

        String main="Display Power Controller:\n  mDisplayId=0\n  mTemporaryScreenBrightness:NaN\n";
        String rear="Display Power Controller:\n  mDisplayId=1\n  mTemporaryScreenBrightness:0.8\n";
        check(InteractionGuard.temporary("Preamble:\n  mDisplayId=0\n"+main+rear)==0,"ignore preamble and rear display");
        check(InteractionGuard.temporary(main.replace(":NaN",":0.008016604"))==1,"active drag");
        check(InteractionGuard.temporary(main.replace(":NaN","=NaN"))==0,"older format");
        check(InteractionGuard.temporary(main+main)==-1,"ambiguous main controller");
        check(InteractionGuard.temporary(rear)==-1,"rear never authorizes main output");
        check(InteractionGuard.temporary(main+"  mTemporaryScreenBrightness:0.1\n")==-1,"duplicate field");
        for(String bad:new String[]{"-1","Infinity","unknown","1.1"})
            check(InteractionGuard.temporary(main.replace("NaN",bad))==-1,"invalid value "+bad);

        check(EventGate.controlIntent(11500,10000,12000,0,0,"main","main"),"recent main-user event suspends regardless of learning capability");
        check(!EventGate.accept(11500,10000,12000,0,0,"main","main",null,null,8,1,false,true),"missing lux still cannot teach");
        check(!EventGate.accept(11500,10000,12000,0,0,"main","main",new float[]{0},new long[]{11000},8,1,false,false),"learning disabled still cannot teach");
        check(!EventGate.controlIntent(8000,7000,12000,0,0,"main","main"),"old event cannot extend interaction");
        check(!EventGate.controlIntent(11500,10000,12000,1,0,"main","main"),"other user");
        check(!EventGate.controlIntent(11500,10000,12000,0,0,"rear","main"),"other display");
        System.out.println("Live drag ownership, long holds, echo filtering, probe races and intent/learning separation passed");
    }
}
