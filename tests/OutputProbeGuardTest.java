// SPDX-License-Identifier: GPL-3.0-only
import org.ref1nd.brightness.OutputProbeGuard;
import static org.ref1nd.brightness.OutputProbeGuard.Result.UNKNOWN;
import static org.ref1nd.brightness.OutputProbeGuard.State.*;

public final class OutputProbeGuardTest {
    static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
    public static void main(String[] args) {
        var g=new OutputProbeGuard();
        check(g.state(0,false)==WAIT,"startup waits without inventing unsafe output");
        var first=g.begin(0);
        check(first!=null&&g.begin(1000)==null,"only one probe per generation in flight");
        check(g.state(4999,false)==WAIT,"slow startup probe is a wait, not an unsafe result");
        check(g.complete(first,5000,OutputProbeGuard.Result.SAFE),"five-second probe accepted");
        check(g.completedAt()==5000&&g.state(5000,true)==OutputProbeGuard.State.SAFE,"freshness starts at completion");
        var next=g.begin(6000);
        check(next!=null&&g.state(10000,true)==OutputProbeGuard.State.SAFE,"pending probe does not discard fresh success");
        check(g.complete(next,11000,UNKNOWN),"a timeout is represented as unknown");
        check(g.state(11000,true)==WAIT,"one timeout waits through the six-second freshness boundary");
        check(g.state(11001,true)==FAULT,"active output without credible evidence eventually faults");
        check(g.begin(12000)==null,"fault does not restart periodic probing and acquisition");
        g.invalidate();
        check(g.state(13000,false)==FAULT,"sleep and wake cannot erase the fault latch");

        g=new OutputProbeGuard();
        first=g.begin(0);g.complete(first,100,OutputProbeGuard.Result.SAFE);g.state(100,true);
        next=g.begin(1000);g.complete(next,1500,UNKNOWN);
        check(g.state(1500,true)==WAIT,"parse failure pauses rather than claiming known unsafe");
        next=g.begin(2000);g.complete(next,2200,OutputProbeGuard.Result.SAFE);
        check(g.state(2200,true)==OutputProbeGuard.State.SAFE,"fresh success recovers a brief pending wait");
        next=g.begin(3000);g.complete(next,3100,OutputProbeGuard.Result.UNSAFE);
        check(g.state(3100,true)==OutputProbeGuard.State.UNSAFE,"explicit unsafe is immediate");
        next=g.begin(4000);g.complete(next,4100,UNKNOWN);
        check(g.state(4100,false)==WAIT,"unknown cannot masquerade as a new explicit unsafe result");
        check(g.state(9101,false)==FAULT,"marking output inactive cannot restart the timeout indefinitely");

        g=new OutputProbeGuard();
        first=g.begin(0);
        long oldGeneration=first.generation;
        g.invalidate();
        next=g.begin(1);
        check(next!=null&&next.generation!=oldGeneration,"wake creates a distinct probe generation");
        check(!g.complete(first,2,OutputProbeGuard.Result.UNSAFE),"old-display unsafe result is rejected");
        check(g.pending()&&g.state(2,false)==WAIT,"stale completion does not clear the new in-flight probe");
        check(g.complete(next,3,OutputProbeGuard.Result.SAFE),"current generation completes");
        check(!g.complete(next,4,OutputProbeGuard.Result.UNSAFE),"duplicate callback cannot replace accepted evidence");
        check(g.begin(1000)==null&&g.begin(1001)!=null,"probe cadence remains bounded");

        g=new OutputProbeGuard();
        first=g.begin(0);g.complete(first,100,OutputProbeGuard.Result.SAFE);g.state(100,true);
        next=g.begin(1000);
        check(!g.complete(next,6101,OutputProbeGuard.Result.SAFE),"late success cannot erase an already expired active deadline");
        check(g.state(6101,true)==FAULT,"late success remains faulted");

        g=new OutputProbeGuard();
        first=g.begin(0);g.complete(first,100,OutputProbeGuard.Result.SAFE);g.state(100,true);
        g.invalidate();
        check(g.state(20000,false)==WAIT,"suspended inactive session does not accumulate a probe fault");
        check(g.state(20000,true)==WAIT&&g.state(26000,true)==WAIT,"wake grants one bounded verification window");
        check(g.state(26001,true)==FAULT,"new active session cannot wait without a limit");

        g=new OutputProbeGuard();
        first=g.begin(0);
        check(g.state(150,true)==WAIT,"main loop may run after a worker has completed");
        check(g.complete(first,100,OutputProbeGuard.Result.SAFE),"delayed callback retains the true worker completion time");
        check(g.lastCompletedAt()==100&&g.state(150,true)==OutputProbeGuard.State.SAFE,"callback delivery order is not a clock failure");

        g=new OutputProbeGuard();
        first=g.begin(0);g.complete(first,100,OutputProbeGuard.Result.SAFE);g.state(100,true);
        for(int i=1;i<=6;i++) {
            next=g.begin(i*1000);
            check(next!=null&&g.complete(next,i*1000+100,UNKNOWN),"short unknown responses can be retried within the bounded window");
            check(g.state(i*1000+100,true)==WAIT,"unknown responses never assert unsafe");
        }
        check(g.state(6101,true)==FAULT,"repeated unknown results do not renew the last credible deadline");

        g=new OutputProbeGuard();
        first=g.begin(0);g.complete(first,100,OutputProbeGuard.Result.SAFE);g.state(100,true);
        next=g.begin(1000);
        check(g.state(6101,true)==FAULT,"main loop may expire while completion is queued");
        check(!g.complete(next,6000,OutputProbeGuard.Result.SAFE),"an earlier worker timestamp cannot undo a fault already observed by the main loop");
        System.out.println("Output probe completion time, generations, waits, explicit unsafe and fault latch passed");
    }
}
