// SPDX-License-Identifier: GPL-3.0-only
import org.ref1nd.brightness.DirectBacklight;
import java.nio.file.*;
import java.io.IOException;

public final class DirectBacklightTest {
    static void check(boolean b,String message){if(!b)throw new AssertionError(message);}
    static void set(Path p,String name,long value)throws Exception{Files.writeString(p.resolve(name),value+"\n");}
    static void output(Path p,long value)throws Exception{set(p,"brightness",value);set(p,"actual_brightness",value);}
    static DirectBacklight writer(Path root,Path journal)throws Exception{return new DirectBacklight(root.toFile(),"panel0",journal.toFile(),"main","boot");}
    public static void main(String[] args)throws Exception {
        Path root=Files.createTempDirectory("iab-direct"),panel=Files.createDirectories(root.resolve("panel0")),journal=root.resolve("recovery");
        try {
            set(panel,"max_brightness",1000);output(panel,200);
            var d=writer(root,journal);
            check(!d.ready(0,.2f)&&d.ready(1000,.2f),"stable framework/node match before first write");
            check(d.write(.3f,.25f)&&d.requested()==250,"live cap clamps write");set(panel,"actual_brightness",250);
            check(d.ready(1100,.2f),"framework target need not equal owned output");
            DirectBacklight.restore(root.toFile(),journal.toFile(),"main","boot",true,false,.4f,1);
            check(d.requested()==250,"legacy protected recovery never raises output");
            d.released();output(panel,200);d.ready(2000,.2f);d.ready(3000,.2f);
            d.write(.3f,1);set(panel,"actual_brightness",300);
            DirectBacklight.restore(root.toFile(),journal.toFile(),"main","boot",true,true,.4f,.35f);
            check(d.requested()==350,"legacy journal still restores current framework with cap");
            d.released();output(panel,200);d.ready(4000,.2f);d.ready(5000,.2f);d.write(.3f,1);set(panel,"actual_brightness",300);
            output(panel,150);
            check(!d.ready(5100,.2f)&&d.faulted()&&!d.owned(),"strict adapter latches external writer conflict");
            check(!journal.toFile().exists(),"conflict cannot restore stale brightness on exit");
            d.released();output(panel,200);
            check(!d.ready(60000,.2f)&&!d.write(.4f,1)&&d.requested()==200,"ordinary release and elapsed time cannot clear fault");

            d=writer(root,journal);d.ready(0,.2f);d.ready(1000,.2f);d.write(.3f,1);
            set(panel,"actual_brightness",100);
            check(!d.ready(1100,.2f)&&d.holding()&&!d.faulted()&&d.owned(),"first mismatch pauses without destroying ownership");
            check(!d.write(.4f,1)&&d.requested()==300,"no writes during unverified driver transition");
            set(panel,"actual_brightness",200);
            check(!d.ready(1500,.2f)&&!d.faulted(),"moving driver is not a stable clamp");
            set(panel,"actual_brightness",300);
            check(d.ready(1600,.2f)&&!d.holding(),"coherent recovery resumes without re-acquisition");
            set(panel,"actual_brightness",100);d.ready(1700,.2f);
            check(!d.ready(2200,.2f)&&d.faulted()&&d.learning.protection.equals("driver_output_mismatch"),"stable 500ms mismatch latches protection");
            output(panel,300);d.released();check(!d.ready(10000,.3f),"matching recovery cannot clear latched clamp");

            // Coherent request/driver/request snapshots reject reads across a ramp.
            output(panel,200);final long[] driver={200};final boolean[] mutate={false};
            DirectBacklight.Readback readback=()->{
                if(mutate[0]){mutate[0]=false;driver[0]+=50;Files.writeString(panel.resolve("brightness"),driver[0]+"\n");}
                return driver[0];
            };
            var reference=new DirectBacklight(root.toFile(),"panel0",journal.toFile(),"main","boot",readback,null,true);
            reference.referenceMode();reference.ready(20000,.8f);check(reference.ready(21000,.8f),"verified scale accepts stale framework");
            check(reference.write(.3f,1)&&reference.requested()==200,"first acquisition preserves live baseline");
            check(reference.ready(21100,.8f)&&reference.write(.3f,1),"next changed target can advance from baseline");driver[0]=300;
            reference.ready(21200,.8f);
            mutate[0]=true;
            check(!reference.ready(21300,.8f)&&reference.holding()&&!reference.faulted()&&reference.owned(),"request changed during driver read only holds");
            check(reference.learning.protection.isEmpty(),"torn snapshot is not persistent protection");
            check(reference.ready(21400,.8f),"coherent snapshot resumes reference writer");
            check(DirectBacklight.units(reference.frameBrightness(.8f),1000)==350,"controller sees live value, not cached own write");
            check(reference.write(.3f,1)&&reference.requested()==350,"unchanged target never chases ordinary system write");
            mutate[0]=true;
            check(!reference.write(.4f,1)&&reference.holding()&&!reference.faulted()&&reference.requested()==400,"last-moment torn write check pauses without exception");
            check(reference.ready(21500,.8f)&&reference.write(.4f,1),"later coherent decision permits changed target");driver[0]=400;
            DirectBacklight.restore(root.toFile(),journal.toFile(),"main","boot",true,true,.8f,1,readback);
            check(reference.requested()==400&&!journal.toFile().exists(),"reference journal never restores stale framework brightness");
            reference.released();reference.ready(22000,.8f);reference.ready(23000,.8f);reference.write(.2f,1);
            DirectBacklight.restore(root.toFile(),journal.toFile(),"main","boot",false,true,.9f,1,readback);
            check(reference.requested()==400&&!journal.toFile().exists(),"screen-off recovery never writes reference panel");reference.released();

            // A coherent but changed value between ready and write is also a race.
            output(panel,200);d=writer(root,journal);d.ready(24000,.2f);d.ready(25000,.2f);output(panel,250);
            check(!d.write(.3f,1)&&d.holding()&&!d.faulted()&&d.requested()==250,"write race is preserved without throwing");
            check(!d.ready(25100,.25f)&&d.ready(26100,.25f),"unowned race needs a new stable baseline");
            Files.delete(panel.resolve("actual_brightness"));
            check(!d.ready(26200,.25f)&&d.faulted(),"real read failure faults immediately");
            output(panel,250);d.released();check(!d.ready(30000,.25f),"read failure also requires explicit restart");

            // Independent driver readback is required for a proven compensated scale.
            set(panel,"max_brightness",16383);set(panel,"brightness",384);set(panel,"actual_brightness",0);driver[0]=384;
            var strict=new DirectBacklight(root.toFile(),"panel0",journal.toFile(),"main","boot",readback);
            check(!strict.ready(0,357f/16383)&&!strict.ready(2000,357f/16383),"generic adapter cannot bypass mapping validation");
            var verified=new DirectBacklight(root.toFile(),"panel0",journal.toFile(),"main","boot",readback,null,true);
            check(verified.frameBrightness(357f/16383)==357f/16383,"no trusted sample means framework fallback");
            verified.ready(3000,357f/16383);check(verified.ready(4000,357f/16383),"verified scale still needs stability");
            check(DirectBacklight.units(verified.frameBrightness(.8f),16383)==384,"first controller frame uses measured output");
            verified.write(.01f,300f/16383);driver[0]=300;
            check(verified.requested()==300,"initial baseline still obeys lower live cap");
            set(panel,"max_brightness",4095);
            check(!verified.ready(4100,357f/16383)&&verified.faulted(),"changed panel range latches immediately");
            try{new DirectBacklight(root.toFile(),"panel0",journal.toFile(),"main","boot",null,null,true);throw new AssertionError("missing driver");}catch(IOException expected){}
            String dump="DisplayDeviceInfo{uniqueId=\"main\"} mBrightness=[0, 0.2, 1], mBacklight=[0, 0.2, 1] DisplayDeviceInfo{uniqueId=\"rear\"} mBrightness=[0, 1], mBacklight=[0.1, 0.9]";
            check(DirectBacklight.identityMapping(dump,"main"),"verified mapping accepted");
            check(!DirectBacklight.identityMapping(dump,"rear")&&!DirectBacklight.identityMapping(dump,"missing"),"nonidentity/missing panel rejected");
            for(String name:new String[]{"../panel0","/sys/panel0",""})try{new DirectBacklight(root.toFile(),name,journal.toFile(),"main","boot");throw new AssertionError("bad path");}catch(IOException expected){}
            for(float value:new float[]{Float.NaN,Float.POSITIVE_INFINITY,-.1f,1.1f})try{DirectBacklight.units(value,1000);throw new AssertionError("bad value");}catch(IllegalArgumentException expected){}
            System.out.println("Direct output coherent sampling, transient holds, latched faults, change-driven writes and recovery passed");
        }finally{try(var paths=Files.walk(root)){for(Path p:paths.sorted(java.util.Comparator.reverseOrder()).toList())Files.delete(p);}}
    }
}
