// SPDX-License-Identifier: GPL-3.0-only
import org.ref1nd.brightness.ReferenceSliderSampling;
import static org.ref1nd.brightness.ReferenceSliderSampling.Action.*;

public final class ReferenceSliderSamplingTest {
    static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
    public static void main(String[] args) {
        var sampling=new ReferenceSliderSampling();
        check(sampling.observe(0,10,true)==BASELINE,"initial setting is only a baseline");
        check(sampling.observe(1000,25,true)==NONE,"drag is not learned immediately");
        check(sampling.observe(7999,25,true)==NONE,"eight-second cadence");
        check(sampling.observe(8000,25,true)==SAMPLE,"settled adjacent sample");
        check(sampling.observe(16000,45,true)==NONE,"still dragging at sampling deadline");
        check(sampling.observe(17000,55,true)==NONE,"drag extends settling");
        check(sampling.observe(18499,55,true)==NONE,"settling required");
        check(sampling.observe(18500,55,true)==SAMPLE,"final stable value only");
        check(sampling.observe(19000,80,false)==BASELINE,"DIM or protection discards change");
        check(sampling.observe(20000,100,false)==BASELINE,"changes during pause are baselines");
        check(sampling.observe(21000,100,true)==BASELINE,"resume cannot learn paused changes");
        check(sampling.observe(28999,100,true)==NONE,"resume starts a fresh period");
        check(sampling.observe(29000,100,true)==SAMPLE,"unchanged sample rebases in core");
        sampling.reset();
        check(sampling.observe(30000,130,true)==BASELINE,"source/wake boundary resets");
        check(sampling.observe(30001,300,true)==NONE,"unknown slider scale rejected");
    }
}
