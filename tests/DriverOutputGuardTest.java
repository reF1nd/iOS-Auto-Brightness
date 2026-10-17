// SPDX-License-Identifier: GPL-3.0-only
import org.ref1nd.brightness.DriverOutputGuard;

public final class DriverOutputGuardTest {
    static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
    public static void main(String[] args) {
        var g=new DriverOutputGuard();
        g.sample(0,200,200,1000,.2f);
        check(!g.learningReady(0),"one matching sample cannot authorize learning");
        g.sample(500,200,200,1000,.2f);
        check(g.learningReady(500),"stable output authorizes confirmation");
        // Drag/ramp samples retain intent/history, but cannot be committed.
        for(int i=1;i<=20;i++) {
            g.sample(500+i*100,200+i*10,195+i*10,1000,.2f+i*.01f);
            check(g.protection.isEmpty()&&!g.learningReady(500+i*100),"moving ramp is pending, never learnable or persistent protection");
        }
        g.sample(2600,400,400,1000,.4f);
        g.sample(3000,400,400,1000,.4f);
        check(!g.learningReady(3000),"recovery requires a fresh full confirmation period");
        g.sample(3100,400,400,1000,.4f);
        check(g.learningReady(3100),"settled drag can be learned");
        // Stable driver clamp cannot be learned, even if the framework cap is absent.
        g.sample(3200,400,300,1000,.4f);
        check(g.protection.isEmpty()&&!g.learningReady(3200),"first mismatch suspends confirmation immediately");
        g.sample(3700,400,300,1000,.4f);
        check(g.protection.equals("driver_output_mismatch")&&!g.learningReady(3700),"sustained mismatch cancels learning");
        g.sample(3800,400,400,1000,.4f);
        check(!g.learningReady(3800),"one recovery sample insufficient");
        g.sample(4300,400,401,1000,.4f);
        check(g.learningReady(4300),"one integer unit readback tolerance");
        check(!g.learningReady(5301),"stale confirmation not reusable");
        g.sample(6000,400,400,1000,.4f);
        check(!g.learningReady(6000),"observation gap cannot stand in for stable samples");
        g.sample(6500,400,-1,1000,.4f);
        check(g.protection.equals("backlight_readback_unavailable"),"missing driver remains immediately protected");
        g.sample(6600,400,400,-1,.4f);
        check(!g.protection.isEmpty(),"changed/unknown range remains immediately protected");
        g.sample(6700,400,400,1000,Float.NaN);
        check(!g.protection.isEmpty(),"unknown framework value cannot authorize learning");
        g.sample(6800,400,400,1000,.4f);g.sample(7300,400,400,1000,.4f);
        g.reset();g.sample(8000,400,400,1000,.4f);
        check(!g.learningReady(8000),"new screen/user session starts unverified");
        g.sample(7900,400,400,1000,.4f);
        check(!g.learningReady(7900),"clock rollback does not count as stability");
        g.reset();
        g.sample(9000,200,250,250,1000,.4f);
        check(g.protection.isEmpty()&&!g.learningReady(9000),"request changing across driver read is not a clamp");
        g.sample(9800,250,300,300,1000,.4f);
        check(g.protection.isEmpty(),"repeated torn snapshots never accumulate stable mismatch time");
        g.sample(10600,300,300,300,1000,.4f);
        check(!g.learningReady(10600),"first coherent recovery restarts confirmation");
        g.sample(11400,300,300,300,1000,.4f);
        check(g.learningReady(11400),"800ms control cadence can confirm stable output");
        g.sample(12200,400,301,400,1000,.4f);
        g.sample(13000,400,302,400,1000,.4f);
        check(g.protection.isEmpty(),"slowly moving driver must not be mistaken for a stationary clamp");
        g.sample(13800,400,302,400,1000,.4f);
        check(g.protection.equals("driver_output_mismatch"),"unchanged driver mismatch over one control period is confirmed");
        System.out.println("Driver learning ramp, clamp, recovery, stale samples and session checks passed");
    }
}
