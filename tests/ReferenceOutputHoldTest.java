// SPDX-License-Identifier: GPL-3.0-only
import org.ref1nd.brightness.ProtectionPolicy;
import org.ref1nd.brightness.ReferenceOutputHold;
import org.ref1nd.brightness.ReferenceSliderSampling;

public final class ReferenceOutputHoldTest {
    static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
    static final String MAIN="Display Power Controller:\n  mDisplayId=0\n  mBrightnessReason=automatic [ dim ]\n  mTemporaryScreenBrightness:NaN\n  mCachedBrightnessInfo.brightnessMaxReason =0\n";
    public static void main(String[] args) {
        check(ProtectionPolicy.referenceDimOutput(MAIN),"main automatic DIM recognized");
        check(ProtectionPolicy.referenceDimOutput(MAIN.replace("[ dim ]","[ bcbc dim ]")),"known compensation may accompany DIM");
        check(ProtectionPolicy.referenceDimOutput(MAIN.replace("=automatic","=temporary")),"temporary request can coexist with DIM");
        for(String dump:new String[]{null,"",MAIN+MAIN,MAIN.replace("mDisplayId=0","mDisplayId=1"),
            MAIN+"  mDisplayId=1\n",MAIN+"  mBrightnessReason=automatic\n",MAIN+"  mBrightnessReason=unknown\n",
            MAIN+"  mCachedBrightnessInfo.brightnessMaxReason =0\n",MAIN.replace("[ dim ]","[ bcbc ]"),
            MAIN.replace("[ dim ]","[ dim vendor ]"),MAIN.replace("[ dim ]","[ dim thermal ]"),
            MAIN.replace("=automatic","=manual"),MAIN.replace("Reason =0","Reason =1"),
            MAIN.replace("Reason =0","Reason :0"),MAIN.replace("mCachedBrightnessInfo.brightnessMaxReason","mCachedBrightnessInfoXbrightnessMaxReason")})
            check(!ProtectionPolicy.referenceDimOutput(dump),"missing, other-display, protected and ambiguous evidence cannot invent DIM");
        String rear=MAIN.replace("mDisplayId=0","mDisplayId=1");
        check(ProtectionPolicy.referenceDimOutput(rear+MAIN),"only the main controller authorizes its DIM state");
        check(!ProtectionPolicy.referenceDimOutput(rear+MAIN.replace("[ dim ]","[ bcbc ]")),"rear DIM must not pause the main panel");

        var hold=new ReferenceOutputHold();
        check(!hold.holdFrames()&&!hold.holdWrites(),"initial output is not paused");
        hold.observe(0,false,false,true);
        hold.observe(100,true,false,false);
        check(hold.dragging()&&hold.awaitingSample()&&hold.holdFrames()&&hold.holdWrites(),"drag pauses frames and writes");
        check(!hold.permitFrameForLearning()&&hold.resumedAt()<0,"never learn intermediate drag output");
        hold.observe(200,false,false,false);
        check(hold.dragging()&&hold.holdFrames(),"unknown cannot terminate a drag");
        hold.observe(300,false,false,true);
        check(!hold.holdFrames()&&hold.holdWrites()&&hold.permitFrameForLearning(),"settled system request permits frames but retains writes for learning");
        check(hold.resumedAt()==300,"release time recorded");
        hold.observe(400,false,false,false);
        check(hold.holdWrites()&&!hold.permitFrameForLearning(),"unknown does not authorize pending learning");
        hold.observe(500,false,false,true);
        check(hold.resumedAt()==300,"repeated ordinary evidence does not restart the sample cadence");
        hold.sampleFinished();
        check(!hold.holdWrites()&&hold.resumedAt()<0,"processed sample resumes writes");
        hold.observe(600,false,true,false);
        check(hold.holdFrames()&&!hold.awaitingSample()&&hold.reason().equals("reference_dim_active"),"DIM pauses without inventing a user sample");
        hold.observe(700,false,false,false);
        check(hold.holdFrames(),"unknown cannot terminate DIM");
        hold.observe(800,false,false,true);
        check(!hold.holdFrames()&&!hold.holdWrites(),"ordinary evidence ends DIM without a learning wait");
        hold.observe(900,true,true,true);
        check(hold.dragging()&&hold.holdFrames()&&!hold.permitFrameForLearning(),"active requests win over a contradictory ordinary flag");
        hold.sampleFinished();
        check(hold.holdWrites()&&!hold.awaitingSample(),"disabled learning never clears an active slider/DIM pause");
        hold.observe(1000,false,false,true);
        check(!hold.holdWrites(),"explicitly skipped learning resumes once the active request ends");
        check(!hold.observe(999,true,false,false)&&!hold.holdWrites(),"older observation cannot resurrect a completed drag");
        hold.observe(1100,true,false,false);hold.reset();
        check(!hold.holdFrames()&&!hold.holdWrites()&&hold.resumedAt()<0,"screen/user boundary clears the session");

        // Bridge must skip observe(false) during TEMP: that would consume the
        // pre-drag baseline. Its existing sampler still owns cadence/stability.
        var sampling=new ReferenceSliderSampling();
        check(sampling.observe(0,20,true)==ReferenceSliderSampling.Action.BASELINE,"initial baseline");
        hold.observe(100,true,false,false);
        hold.observe(9000,false,false,true);
        check(hold.permitFrameForLearning()&&hold.holdWrites(),"long drag still waits for its final sample");
        check(sampling.observe(9000,40,true)==ReferenceSliderSampling.Action.NONE,"release value starts stability check without replacing baseline");
        check(sampling.observe(10499,40,true)==ReferenceSliderSampling.Action.NONE&&hold.holdWrites(),"wait full 1500 ms");
        check(sampling.observe(10500,40,true)==ReferenceSliderSampling.Action.SAMPLE,"settled value reaches the sample callback");
        hold.sampleFinished();
        check(!hold.holdWrites(),"sample completion releases only the write pause");
        System.out.println("Reference slider/DIM pauses preserve ownership and settled learning cadence passed");
    }
}
