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
            final String[] mode={"rw-r--r--"};
            var access=new BacklightLease.Access(){
                public String get(File f){return mode[0];}
                public void set(File f,String m){mode[0]=m;}
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
            check(!d.ready(3100,.2f)&&!d.locked()&&mode[0].equals("rw-r--r--"),"driver mismatch releases permissions immediately");
            set(panel,"brightness",200);set(panel,"actual_brightness",200);
            d.ready(6000,.2f);d.ready(7000,.2f);d.write(.3f,1);set(panel,"actual_brightness",300);
            mode[0]="rw-rw-r--";
            check(!d.ready(7100,.2f)&&mode[0].equals("rw-rw-r--"),"external unlock is not fought");
            // Actual chmod plus actual writes as the unprivileged Actions user:
            // reopening 0444 fails, a descriptor opened before locking survives.
            posix.set(file,"rw-r--r--");set(panel,"brightness",200);set(panel,"actual_brightness",200);
            var live=new DirectBacklight(root.toFile(),"panel0",root.resolve("direct-output.properties").toFile(),"main","boot",null,posix);
            live.ready(8000,.2f);live.ready(9000,.2f);
            live.write(.3f,1);set(panel,"actual_brightness",300);
            check(posix.get(file).equals("r--r--r--")&&Files.readString(file.toPath()).trim().equals("300"),"retained descriptor writes after real chmod");
            live.write(.31f,1);set(panel,"actual_brightness",310);
            check(Files.readString(file.toPath()).trim().equals("310"),"subsequent write seeks to start without reopening");
            live.released();check(posix.get(file).equals("rw-r--r--"),"close retained descriptor and restore mode");
            System.out.println("Backlight permission acquisition, normal/crash recovery, foreign ownership and direct output passed");
        }finally{try(var paths=Files.walk(root)){for(Path p:paths.sorted(java.util.Comparator.reverseOrder()).toList())Files.delete(p);}}
    }
}
