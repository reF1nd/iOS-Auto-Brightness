// SPDX-License-Identifier: GPL-3.0-only
import org.ref1nd.brightness.BacklightLease;
import org.ref1nd.brightness.DirectBacklight;
import java.nio.file.*;
import java.io.*;

public final class BacklightLeaseTest {
    static void check(boolean b,String m){if(!b)throw new AssertionError(m);}
    static void set(Path p,String name,long v)throws IOException{Files.writeString(p.resolve(name),v+"\n");}
    public static void main(String[] args)throws Exception {
        Path root=Files.createTempDirectory("iab-lease"),panel=Files.createDirectories(root.resolve("panel0"));
        File file=panel.resolve("brightness").toFile(),journal=root.resolve("backlight-permissions.properties").toFile();
        var posix=BacklightLease.POSIX;
        try {
            set(panel,"brightness",200);posix.set(file,"rw-rw-r--");
            var lease=new BacklightLease(root.toFile(),"panel0",journal,"boot",posix);
            lease.acquire();check(lease.intact()&&journal.isFile(),"lease and recovery persisted");
            lease.release();check(posix.get(file).equals("rw-rw-r--")&&!journal.exists(),"restore original mode, not hardcoded 0644");
            lease.acquire();
            BacklightLease.restore(root.toFile(),journal,"boot",posix);
            check(posix.get(file).equals("rw-rw-r--"),"fresh recovery instance handles crashed owner without display APIs");
            lease.release();lease.acquire();
            var failing=new BacklightLease.Access(){
                public String get(File f)throws IOException{return posix.get(f);}
                public void set(File f,String m)throws IOException{throw new IOException("simulated restore failure");}
            };
            try{BacklightLease.restore(root.toFile(),journal,"boot",failing);throw new AssertionError("restore failure");}catch(IOException expected){}
            check(journal.isFile(),"failed recovery retains durable retry data");
            BacklightLease.restore(root.toFile(),journal,"boot",posix);
            check(posix.get(file).equals("rw-rw-r--"),"supervisor retry can restore after failure");
            lease.release();lease.acquire();posix.set(file,"rw-------");lease.release();
            check(posix.get(file).equals("rw-------"),"external permission change wins");
            posix.set(file,"r--r--r--");
            try{lease.acquire();throw new AssertionError("foreign lock");}catch(IOException expected){}
            check(!journal.exists(),"never claim someone else's readonly node");
            posix.set(file,"rw-r--r--");lease.acquire();
            BacklightLease.restore(root.toFile(),journal,"different-boot",posix);
            check(posix.get(file).equals("r--r--r--")&&!journal.exists(),"stale boot never changes current kernel node");
            posix.set(file,"rw-r--r--");
            // Fake only chmod for the non-root host writer; real chmod/recovery is
            // exercised above. This permits real file I/O without CAP_DAC_OVERRIDE.
            final String[] mode={"rw-r--r--"};final int[] changes={0};
            var access=new BacklightLease.Access(){
                public String get(File f){return mode[0];}
                public void set(File f,String m){mode[0]=m;changes[0]++;}
            };
            set(panel,"max_brightness",1000);set(panel,"brightness",200);set(panel,"actual_brightness",200);
            var d=new DirectBacklight(root.toFile(),"panel0",root.resolve("direct-output.properties").toFile(),"main","boot",null,access);
            d.ready(0,.2f);check(d.ready(1000,.2f),"verified mapping");
            d.write(.3f,1);set(panel,"actual_brightness",300);
            check(d.locked()&&mode[0].equals("r--r--r--"),"first write locks ordinary writers");
            check(d.ready(1100,.21f),"framework BCBC target does not invalidate owned node mapping");
            d.write(.31f,1);set(panel,"actual_brightness",310);
            check(d.locked(),"successive output keeps single lease");
            d.released();check(mode[0].equals("rw-r--r--")&&!journal.exists(),"release unlocks");
            set(panel,"brightness",200);set(panel,"actual_brightness",200);
            d.ready(2000,.2f);d.ready(3000,.2f);d.write(.3f,1);set(panel,"actual_brightness",150);
            check(!d.ready(3100,.2f)&&d.holding()&&d.locked()&&mode[0].equals("r--r--r--"),"transient mismatch retains permissions without writing");
            check(!d.ready(3600,.2f)&&d.faulted()&&!d.locked()&&mode[0].equals("rw-r--r--"),"stable mismatch releases exactly once");
            int changesAfterFault=changes[0];d.released();d.ready(5000,.2f);
            check(changes[0]==changesAfterFault,"ordinary cleanup never reacquires or repeatedly releases faulted lease");
            set(panel,"brightness",200);set(panel,"actual_brightness",200);
            d=new DirectBacklight(root.toFile(),"panel0",root.resolve("direct-output.properties").toFile(),"main","boot",null,access);
            d.ready(6000,.2f);d.ready(7000,.2f);d.write(.3f,1);set(panel,"actual_brightness",300);
            mode[0]="rw-rw-r--";
            check(!d.ready(7100,.2f)&&d.faulted()&&mode[0].equals("rw-rw-r--"),"external unlock is not fought and fault is latched");
            // Actual chmod plus actual writes as the unprivileged Actions user:
            // reopening 0444 fails, a descriptor opened before locking survives.
            posix.set(file,"rw-r--r--");set(panel,"brightness",200);set(panel,"actual_brightness",200);
            var live=new DirectBacklight(root.toFile(),"panel0",root.resolve("direct-output.properties").toFile(),"main","boot",null,posix);
            live.ready(8000,.2f);live.ready(9000,.2f);
            live.write(.3f,1);set(panel,"actual_brightness",300);
            check(posix.get(file).equals("r--r--r--")&&Files.readString(file.toPath()).trim().equals("300"),"retained descriptor writes after real chmod");
            live.ready(9100,.2f);live.write(.31f,1);set(panel,"actual_brightness",310);
            check(Files.readString(file.toPath()).trim().equals("310"),"subsequent write seeks to start without reopening");
            live.released();check(posix.get(file).equals("rw-r--r--"),"close retained descriptor and restore mode");
            set(panel,"brightness",200);set(panel,"actual_brightness",200);mode[0]="rw-r--r--";
            var reference=new DirectBacklight(root.toFile(),"panel0",root.resolve("direct-output.properties").toFile(),"main","boot",()->Long.parseLong(Files.readString(panel.resolve("actual_brightness")).trim()),access,true);
            reference.referenceMode();reference.ready(10000,.8f);check(reference.ready(11000,.8f),"reference scale need not match stale framework");
            reference.write(.3f,1);
            check(reference.requested()==200,"reference acquisition retains live baseline");
            reference.ready(11100,.8f);reference.write(.3f,1);set(panel,"actual_brightness",300);
            check(reference.requested()==300,"changed target advances after acquisition");
            reference.suspend();check(reference.locked(),"sleep retains lease without writing");
            set(panel,"brightness",500);set(panel,"actual_brightness",500);
            check(reference.ready(12000,.8f),"reference ownership does not follow framework transitions");
            reference.write(.3f,1);
            check(reference.requested()==500,"reference never chases external output when target has not changed");
            set(panel,"actual_brightness",100);
            check(!reference.ready(12100,.8f)&&reference.holding()&&reference.locked(),"first driver mismatch only holds reference output");
            check(!reference.ready(12600,.8f)&&reference.faulted()&&!reference.locked(),"confirmed driver mismatch releases reference lease once");
            reference.suspend();reference.released();set(panel,"actual_brightness",500);
            check(!reference.ready(20000,.8f)&&reference.faulted(),"sleep and ordinary release cannot reset fault");

            // A real journal I/O failure restores acquired permissions and cannot retry.
            set(panel,"brightness",200);set(panel,"actual_brightness",200);mode[0]="rw-r--r--";
            var broken=new DirectBacklight(root.toFile(),"panel0",root.resolve("missing/recovery").toFile(),"main","boot",null,access);
            broken.ready(30000,.2f);broken.ready(31000,.2f);
            try{broken.write(.3f,1);throw new AssertionError("journal failure");}catch(IOException expected){}
            check(broken.faulted()&&!broken.locked()&&mode[0].equals("rw-r--r--")&&broken.requested()==200,"I/O failure leaves output untouched and unlocks");
            check(!broken.ready(40000,.2f),"I/O failure cannot automatically retry");
            posix.set(file,"rw-r--r--");set(panel,"brightness",200);set(panel,"actual_brightness",200);
            File outputJournal=root.resolve("direct-output.properties").toFile();
            var referenceLive=new DirectBacklight(root.toFile(),"panel0",outputJournal,"main","boot",()->Long.parseLong(Files.readString(panel.resolve("actual_brightness")).trim()),posix,true);
            referenceLive.referenceMode();referenceLive.ready(41000,.8f);referenceLive.ready(42000,.8f);
            referenceLive.write(.3f,1);referenceLive.ready(42100,.8f);referenceLive.write(.3f,1);set(panel,"actual_brightness",300);
            DirectBacklight.restore(root.toFile(),outputJournal,"main","boot",true,true,.8f,1,()->300);
            check(referenceLive.requested()==300&&posix.get(file).equals("rw-r--r--")&&!outputJournal.exists(),"reference exit restores real permissions without writing stale framework brightness");
            referenceLive.released();
            System.out.println("Backlight permission acquisition, normal/crash recovery, foreign ownership and direct output passed");
        }finally{try(var paths=Files.walk(root)){for(Path p:paths.sorted(java.util.Comparator.reverseOrder()).toList())Files.delete(p);}}
    }
}
