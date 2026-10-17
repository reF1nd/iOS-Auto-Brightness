// SPDX-License-Identifier: GPL-3.0-only
import org.ref1nd.brightness.DirectBacklight;
import org.ref1nd.brightness.ReferenceOutputHold;
import java.nio.file.*;
import java.io.IOException;

public final class DirectBacklightTest {
    static void check(boolean b,String message){if(!b)throw new AssertionError(message);}
    static void set(Path p,String name,long value)throws Exception{Files.writeString(p.resolve(name),value+"\n");}
    static void output(Path p,long value)throws Exception{set(p,"brightness",value);set(p,"actual_brightness",value);}
    static DirectBacklight writer(Path root,Path journal)throws Exception{return new DirectBacklight(root.toFile(),"panel0",journal.toFile(),"main","boot");}
    static DirectBacklight referenceWriter(Path root,Path journal,DirectBacklight.Readback readback)throws Exception {
        var d=new DirectBacklight(root.toFile(),"panel0",journal.toFile(),"main","boot",readback,null,true);
        d.referenceMode();return d;
    }
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
            check(d.ready(5100,.2f)&&d.externalChanged()&&!d.faulted()&&d.owned(),"coherent external output keeps control pending core synchronization");
            check(!d.write(.25f,1)&&d.requested()==150,"unsynchronized external output cannot be overwritten");
            check(d.acknowledgeExternal(d.externalRevision()),"current coherent observation can be acknowledged after core sync");
            check(d.write(.25f,1)&&d.requested()>150&&d.requested()<160,"stale lower curve target cannot become a large physical rise");
            d.released();

            output(panel,200);d=writer(root,journal);d.ready(0,.2f);d.ready(1000,.2f);d.write(.3f,1);
            set(panel,"actual_brightness",100);
            check(!d.ready(1100,.2f)&&d.holding()&&!d.faulted()&&d.owned(),"first mismatch pauses without destroying control");
            check(!d.write(.4f,1)&&d.requested()==300,"no writes during unverified driver transition");
            set(panel,"actual_brightness",200);
            check(!d.ready(1500,.2f)&&!d.faulted(),"moving driver is not a stable clamp");
            set(panel,"actual_brightness",300);
            check(d.ready(1600,.2f)&&!d.holding(),"coherent recovery resumes without reacquiring");
            set(panel,"actual_brightness",100);d.ready(1700,.2f);
            check(!d.ready(2200,.2f)&&d.faulted()&&d.learning.protection.equals("driver_output_mismatch"),"stable 500ms mismatch still latches protection");
            output(panel,300);d.suspend();d.released();
            check(!d.ready(10000,.3f)&&!d.write(.4f,1),"sleep and cleanup cannot clear a genuine driver fault");

            output(panel,200);final long[] driver={200};final boolean[] mutate={false};
            DirectBacklight.Readback readback=()->{
                if(mutate[0]){mutate[0]=false;driver[0]+=50;Files.writeString(panel.resolve("brightness"),driver[0]+"\n");}
                return driver[0];
            };
            var reference=referenceWriter(root,journal,readback);
            reference.ready(20000,.8f);check(reference.ready(21000,.8f),"verified scale accepts stale framework");
            check(reference.write(.3f,1)&&reference.requested()==200,"first acquisition preserves live baseline");
            check(reference.ready(21100,.8f)&&reference.write(.3f,1)&&reference.requested()>200&&reference.requested()<211,"ordinary upward step obeys physical rise insurance");
            driver[0]=reference.lastWritten();reference.ready(21200,.8f);
            mutate[0]=true;
            check(!reference.ready(21300,.8f)&&reference.holding()&&!reference.faulted()&&reference.owned(),"request changed during driver read only holds");
            check(reference.learning.protection.isEmpty(),"torn snapshot is not persistent protection");
            check(reference.ready(21400,.8f)&&reference.externalChanged()&&!reference.faulted(),"coherent external output after a torn read requests core sync");
            long changed=reference.requested(),revision=reference.externalRevision();
            check(DirectBacklight.units(reference.frameBrightness(.8f),1000)==changed,"sync uses actual output rather than cached own write");
            check(!reference.write(.3f,1)&&reference.requested()==changed,"pending observation cannot be bypassed by a changed target");
            check(reference.acknowledgeExternal(revision)&&!reference.externalChanged(),"matching core-sync acknowledgement clears pending observation");
            long oldLast=reference.lastWritten();
            check(reference.write(oldLast/1000f,1)&&!reference.lastWriteChanged()&&reference.lastAcceptedRaw()==changed&&reference.requested()==changed,"same old target stays a no-op and reports actual accepted output");
            output(panel,oldLast);driver[0]=oldLast;
            check(reference.ready(21500,.8f)&&reference.externalChanged()&&reference.externalRevision()>revision,"external return to a previous own write is a new observation");
            check(!reference.acknowledgeExternal(revision),"stale acknowledgement cannot consume a newer observation");
            check(reference.acknowledgeExternal(reference.externalRevision()),"current revision can be synchronized");
            revision=reference.externalRevision();reference.ready(21600,.8f);
            check(!reference.externalChanged()&&reference.externalRevision()==revision,"unchanged coherent samples do not repeatedly restart core sync");
            DirectBacklight.restore(root.toFile(),journal.toFile(),"main","boot",true,true,.8f,1,readback);
            check(reference.requested()==oldLast&&!journal.toFile().exists(),"reference recovery never writes stale framework brightness");reference.released();

            // A write-time race must be synchronized even when its target is unchanged.
            for(float target:new float[]{.3f,.25f}) {
                output(panel,300);driver[0]=300;reference=referenceWriter(root,journal,readback);
                reference.ready(0,.8f);reference.ready(1000,.8f);reference.write(.3f,1);reference.ready(1200,.8f);
                output(panel,150);driver[0]=150;
                check(!reference.write(target,1)&&reference.externalChanged()&&!reference.faulted()&&reference.requested()==150,"write-time coherent race preserves external output and control");
                check(reference.lastAcceptedRaw()<0&&!reference.lastWriteChanged(),"rejected write cannot report a submission");
                check(reference.ready(1300,.8f)&&reference.acknowledgeExternal(reference.externalRevision()),"next frame can synchronize a write-time race");
                check(reference.write(target,1)&&reference.requested()>=150&&reference.requested()<160,"synchronized target either stays unchanged or rises only within the live bound");
            }
            output(panel,300);driver[0]=300;reference=referenceWriter(root,journal,readback);
            reference.ready(0,.8f);reference.ready(1000,.8f);reference.write(.3f,1);reference.ready(1200,.8f);
            mutate[0]=true;
            check(!reference.write(.3f,1)&&reference.holding()&&!reference.faulted(),"unchanged target still rejects torn write-time snapshots");
            check(reference.ready(1300,.8f)&&reference.externalChanged()&&!reference.faulted(),"torn write-time snapshot recovers only through core synchronization");

            output(panel,300);driver[0]=300;reference=referenceWriter(root,journal,readback);
            reference.ready(0,.8f);reference.ready(1000,.8f);reference.write(.3f,1);
            output(panel,500);driver[0]=500;reference.ready(1200,.8f);reference.acknowledgeExternal(reference.externalRevision());
            check(reference.write(.3f,.3f)&&reference.requested()==300&&reference.lastWriteChanged(),"same cached target cannot skip a new cap below external live output");

            // Mutate only in the final snapshot, after early checks and journaling.
            for(int mutationRead:new int[]{2,3}) {
                output(panel,300);final int[] reads={0};final boolean[] armed={false};
                DirectBacklight.Readback late=()->{
                    if(armed[0]&&++reads[0]==mutationRead) {
                        Files.writeString(panel.resolve("brightness"),"150\n");
                        Files.writeString(panel.resolve("actual_brightness"),"150\n");
                    }
                    return Long.parseLong(Files.readString(panel.resolve("actual_brightness")).trim());
                };
                var lateWriter=referenceWriter(root,journal,late);
                lateWriter.ready(0,.8f);lateWriter.ready(1000,.8f);lateWriter.write(.3f,1);
                String firstJournal=Files.readString(journal);
                if(mutationRead==2)lateWriter.ready(1200,.8f);
                armed[0]=true;
                if(mutationRead==3)lateWriter.ready(1200,.8f);
                check(!lateWriter.write(.25f,1)&&lateWriter.holding()&&lateWriter.requested()==150&&!lateWriter.lastWriteChanged(),"final snapshot rejects a late change after the early output check");
                check(lateWriter.ready(1300,.8f)&&lateWriter.externalChanged()&&lateWriter.owned()&&!lateWriter.faulted(),"late write race preserves control until the next coherent sync");
                check(lateWriter.acknowledgeExternal(lateWriter.externalRevision())&&lateWriter.write(.25f,1)&&lateWriter.requested()<160,"late race cannot turn a lower cached target into a large rise");
                check(firstJournal.equals(Files.readString(journal)),"reference ramp and skipped writes never rewrite the durable no-restore journal");
                lateWriter.released();
            }

            // Explicit TEMP/DIM pauses writes while retaining the output claim.
            for(boolean slider:new boolean[]{true,false}) {
                output(panel,300);driver[0]=300;reference=referenceWriter(root,journal,readback);
                reference.ready(0,.8f);reference.ready(1000,.8f);reference.write(.3f,1);
                var hold=new ReferenceOutputHold();output(panel,150);driver[0]=150;
                hold.observe(1200,slider,!slider,false);
                check(hold.holdWrites()&&reference.owned()&&!reference.faulted()&&reference.requested()==150,"explicit slider or DIM pauses writes without discarding control");
                hold.observe(1400,false,false,true);
                check(!hold.holdFrames()&&reference.owned(),"ordinary frame ends the request without reacquiring output");
                if(slider)check(hold.holdWrites()&&hold.permitFrameForLearning(),"a completed drag preserves its actual value until the settled sample");
                hold.sampleFinished();
                check(!hold.holdWrites()&&reference.ready(1500,.8f)&&reference.externalChanged(),"resuming a paused request first synchronizes the current output");
                check(reference.acknowledgeExternal(reference.externalRevision())&&reference.write(.15f,1)&&reference.requested()==150&&!reference.faulted(),"request pause resumes from actual output rather than replaying the old target");reference.released();
            }
            DirectBacklight.restore(root.toFile(),journal.toFile(),"main","boot",false,true,.9f,1,readback);
            check(reference.requested()==150&&!journal.toFile().exists(),"screen-off recovery never writes reference panel");
            output(panel,300);driver[0]=300;reference=referenceWriter(root,journal,readback);
            reference.ready(0,.8f);reference.ready(1000,.8f);reference.write(.3f,1);
            var ordinary=new ReferenceOutputHold();ordinary.observe(1200,false,false,true);
            output(panel,150);driver[0]=150;
            check(!ordinary.holdWrites()&&reference.ready(1400,.8f)&&reference.owned()&&reference.externalChanged()&&!reference.faulted(),"ordinary automatic output synchronizes without becoming a release or permanent fault");
            check(reference.acknowledgeExternal(reference.externalRevision())&&reference.write(.25f,1)&&reference.requested()<160,"ordinary changed output resumes bounded control after sync");reference.released();

            output(panel,200);d=writer(root,journal);d.ready(24000,.2f);d.ready(25000,.2f);output(panel,250);
            check(!d.write(.3f,1)&&d.holding()&&!d.faulted()&&d.requested()==250,"unowned write race is preserved without exception");
            check(!d.ready(25100,.25f)&&d.ready(26100,.25f),"unowned race requires a new stable baseline");
            Files.delete(panel.resolve("actual_brightness"));
            check(!d.ready(26200,.25f)&&d.faulted(),"real read failure still faults immediately");
            output(panel,250);d.released();check(!d.ready(30000,.25f),"read failure requires explicit restart");

            set(panel,"max_brightness",16383);set(panel,"brightness",384);set(panel,"actual_brightness",0);driver[0]=384;
            var strict=new DirectBacklight(root.toFile(),"panel0",journal.toFile(),"main","boot",readback);
            check(!strict.ready(0,357f/16383)&&!strict.ready(2000,357f/16383),"generic adapter cannot bypass mapping validation");
            var verified=new DirectBacklight(root.toFile(),"panel0",journal.toFile(),"main","boot",readback,null,true);
            check(verified.frameBrightness(357f/16383)==357f/16383,"no trusted sample means framework fallback");
            verified.ready(3000,357f/16383);check(verified.ready(4000,357f/16383),"verified scale still needs stability");
            check(DirectBacklight.units(verified.frameBrightness(.8f),16383)==384,"first controller frame uses measured output");
            verified.write(.01f,300f/16383);driver[0]=300;
            check(verified.requested()==300,"initial baseline obeys lower live cap");
            output(panel,2138);driver[0]=2138;reference=referenceWriter(root,journal,readback);
            reference.ready(5000,.028755f);reference.ready(6000,.028755f);reference.write(.130509f,1);
            output(panel,499);driver[0]=499;
            check(reference.ready(6800,.028755f)&&reference.externalChanged()&&reference.owned()&&!reference.faulted(),"beta23 coherent 2138 to 499 change retains control with a pending live sync");
            check(!reference.write(.0795644f,1)&&reference.requested()==499,"old lower curve cannot brighten output to 1304 before synchronization");
            check(reference.acknowledgeExternal(reference.externalRevision())&&reference.write(.0795644f,1),"synchronized lower curve can continue control");
            check(reference.requested()>499&&reference.requested()<530&&reference.lastAcceptedRaw()==reference.requested()&&reference.lastWriteChanged(),"even a stale post-sync proposal cannot make a large physical jump");
            driver[0]=reference.lastWritten();reference.ready(6850,.028755f);
            check(reference.write(.02f,300f/16383)&&reference.requested()==300,"a lower system cap bypasses upward-only insurance");
            driver[0]=300;reference.suspend();output(panel,180);driver[0]=180;
            check(!reference.ready(7000,.028755f)&&reference.ready(8000,.028755f)&&reference.write(.1f,1)&&reference.requested()==180,"wake discards pre-sleep target and reacquires the actual baseline");reference.released();
            set(panel,"max_brightness",4095);
            check(!verified.ready(4100,357f/16383)&&verified.faulted(),"changed panel range latches immediately");
            try{new DirectBacklight(root.toFile(),"panel0",journal.toFile(),"main","boot",null,null,true);throw new AssertionError("missing driver");}catch(IOException expected){}
            String dump="DisplayDeviceInfo{uniqueId=\"main\"} mBrightness=[0, 0.2, 1], mBacklight=[0, 0.2, 1] DisplayDeviceInfo{uniqueId=\"rear\"} mBrightness=[0, 1], mBacklight=[0.1, 0.9]";
            check(DirectBacklight.identityMapping(dump,"main"),"verified mapping accepted");
            check(!DirectBacklight.identityMapping(dump,"rear")&&!DirectBacklight.identityMapping(dump,"missing"),"nonidentity/missing panel rejected");
            for(String name:new String[]{"../panel0","/sys/panel0",""})try{new DirectBacklight(root.toFile(),name,journal.toFile(),"main","boot");throw new AssertionError("bad path");}catch(IOException expected){}
            for(float value:new float[]{Float.NaN,Float.POSITIVE_INFINITY,-.1f,1.1f})try{DirectBacklight.units(value,1000);throw new AssertionError("bad value");}catch(IllegalArgumentException expected){}
            System.out.println("Direct output coherent sampling, live synchronization, bounded rises, protected faults and recovery passed");
        }finally{try(var paths=Files.walk(root)){for(Path p:paths.sorted(java.util.Comparator.reverseOrder()).toList())Files.delete(p);}}
    }
}
