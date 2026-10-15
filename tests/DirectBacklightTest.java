// SPDX-License-Identifier: GPL-3.0-only
import org.ref1nd.brightness.DirectBacklight;
import java.nio.file.*;

public final class DirectBacklightTest {
    static void check(boolean b,String message){if(!b)throw new AssertionError(message);}
    static void set(Path p,String name,long value)throws Exception{Files.writeString(p.resolve(name),value+"\n");}
    public static void main(String[] args)throws Exception {
        Path root=Files.createTempDirectory("iab-direct"),panel=Files.createDirectories(root.resolve("panel0")),journal=root.resolve("recovery");
        try {
            set(panel,"max_brightness",1000);set(panel,"brightness",200);set(panel,"actual_brightness",200);
            var d=new DirectBacklight(root.toFile(),"panel0",journal.toFile(),"main","boot");
            check(!d.ready(0,.2f)&&d.ready(1000,.2f),"stable framework/node match before first write");
            d.write(.3f,.25f);check(Files.readString(panel.resolve("brightness")).trim().equals("250"),"live cap clamps write");
            set(panel,"actual_brightness",250);
            check(d.ready(1100,.2f),"framework deliberately differs from raw output; no false takeover");
            DirectBacklight.restore(root.toFile(),journal.toFile(),"main","boot",true,false,.4f,1);
            check(Files.readString(panel.resolve("brightness")).trim().equals("250"),"protected recovery never raises output");
            d.released();set(panel,"brightness",200);set(panel,"actual_brightness",200);
            check(!d.ready(2000,.2f)&&d.ready(3000,.2f),"reverify after release");d.write(.3f,1);set(panel,"actual_brightness",300);
            set(panel,"brightness",100);set(panel,"actual_brightness",100);
            check(!d.ready(3100,.2f)&&!d.owned(),"external dim wins without reassertion");
            check(!journal.toFile().exists(),"external write cannot be undone by crash recovery");
            check(!d.ready(6000,.2f),"unexplained dim cannot be recaptured despite elapsed time");
            check(!d.ready(6100,.1f)&&d.ready(7100,.1f),"recapture requires fresh stable system mapping");
            d.write(.2f,1);set(panel,"actual_brightness",100);
            check(!d.ready(7200,.1f),"driver cap prevents continued writes even when requested value is ours");
            check(d.learning.protection.isEmpty()&&!d.learning.learningReady(7200),"first mismatch pauses writing and saving without destroying learning history");
            check(!d.ready(7700,.1f)&&d.learning.protection.equals("driver_output_mismatch"),"stable hidden cap becomes learning protection");
            set(panel,"brightness",100);set(panel,"actual_brightness",100);
            d.ready(10000,.1f);check(d.ready(11000,.1f),"mapping settles after driver recovery");
            d.write(.2f,1);set(panel,"actual_brightness",200);
            DirectBacklight.restore(root.toFile(),journal.toFile(),"main","boot",true,true,.3f,.25f);
            check(Files.readString(panel.resolve("brightness")).trim().equals("250"),"normal/crash release restores current framework with live cap");
            d.released();set(panel,"brightness",100);set(panel,"actual_brightness",100);
            d.ready(12000,.1f);d.ready(13000,.1f);d.write(.2f,1);set(panel,"actual_brightness",200);
            set(panel,"brightness",150);set(panel,"actual_brightness",150);
            DirectBacklight.restore(root.toFile(),journal.toFile(),"main","boot",true,true,.3f,1);
            check(Files.readString(panel.resolve("brightness")).trim().equals("150"),"release leaves newer system write untouched");
            d.released();d.ready(14000,.15f);d.ready(15000,.15f);d.write(.2f,1);set(panel,"actual_brightness",200);
            DirectBacklight.restore(root.toFile(),journal.toFile(),"main","boot",false,true,.4f,1);
            check(Files.readString(panel.resolve("brightness")).trim().equals("200")&&!journal.toFile().exists(),"screen off never restores stale screen-on level");
            d.released();set(panel,"brightness",100);set(panel,"actual_brightness",100);
            d.ready(16000,.1f);d.ready(17000,.1f);set(panel,"brightness",150);set(panel,"actual_brightness",150);
            try{d.write(.2f,1);throw new AssertionError("race must fail");}catch(java.io.IOException expected){}
            check(Files.readString(panel.resolve("brightness")).trim().equals("150"),"last-moment external write is preserved");
            // The Qualcomm sysfs actual_brightness callback is a constant-zero stub.
            // A verified adapter must use the separate read-only driver API instead.
            final long[] driver={812};set(panel,"max_brightness",16383);set(panel,"brightness",812);set(panel,"actual_brightness",0);
            var vendor=new DirectBacklight(root.toFile(),"panel0",journal.toFile(),"main","boot",()->driver[0]);
            check(!vendor.ready(20000,(812f/16383))&&vendor.ready(21000,(812f/16383)),"strict live mapping with independent driver readback");
            vendor.write(.04f,.4f);driver[0]=655;
            check(vendor.ready(22000,(812f/16383)),"vendor readback validates raw write despite sysfs zero stub");
            DirectBacklight.restore(root.toFile(),journal.toFile(),"main","boot",true,true,(812f/16383),.4f,()->driver[0]);
            check(Files.readString(panel.resolve("brightness")).trim().equals("812"),"unchanged framework restores exact original system node value");
            vendor.released();driver[0]=812;vendor.ready(23000,(812f/16383));vendor.ready(24000,(812f/16383));vendor.write(.04f,.4f);
            driver[0]=400;check(!vendor.ready(25000,(812f/16383)),"vendor thermal driver reduction must also yield");
            driver[0]=-1;check(!vendor.ready(30000,(812f/16383)),"failed vendor readback never authorizes control");
            check(vendor.learning.protection.equals("backlight_readback_unavailable"),"failed readback immediately invalidates learning");
            // Regression: stable framework 357 vs compensated raw/driver 384.
            vendor.released();driver[0]=384;set(panel,"brightness",384);
            var strict=new DirectBacklight(root.toFile(),"panel0",journal.toFile(),"main","boot",()->driver[0]);
            check(!strict.ready(31000,357f/16383)&&!strict.ready(33000,357f/16383),"readback alone never bypasses generic mapping validation");
            var verified=new DirectBacklight(root.toFile(),"panel0",journal.toFile(),"main","boot",()->driver[0],null,true);
            check(verified.frameBrightness(357f/16383)==357f/16383,"unverified frame keeps framework baseline");
            check(!verified.ready(34000,357f/16383),"verified panel still requires stable output");
            driver[0]=385;set(panel,"brightness",385);
            check(!verified.ready(34900,357f/16383)&&!verified.ready(35000,357f/16383),"node changes restart stability window");
            check(verified.ready(35900,357f/16383),"proven scale accepts stable compensated output");
            check(DirectBacklight.units(verified.frameBrightness(357f/16383),16383)==385,"first controller frame uses actual output");
            verified.write(.01f,1);
            check(verified.requested()==385,"first acquisition preserves live backlight instead of jumping to target");
            verified.write(.02f,1);driver[0]=328;
            check(verified.requested()==328&&verified.ready(36000,357f/16383),"subsequent ramp writes and verifies independent driver output");
            check(DirectBacklight.units(verified.frameBrightness(357f/16383),16383)==328,"owned frames use module output");
            verified.released();driver[0]=384;set(panel,"brightness",384);
            verified.ready(37000,357f/16383);verified.ready(38000,357f/16383);
            verified.write(.01f,300f/16383);driver[0]=300;
            check(verified.requested()==300,"first acquisition still honors a lower system cap");
            set(panel,"brightness",301);driver[0]=301;
            check(!verified.ready(38100,357f/16383)&&!verified.owned(),"verified scale never ignores external writes");
            check(!verified.ready(39000,357f/16383)&&verified.ready(40200,357f/16383),"external writer retry delay remains enforced");
            driver[0]=200;check(!verified.ready(41000,357f/16383),"driver clamp still blocks verified adapter");
            driver[0]=301;set(panel,"max_brightness",4095);
            check(!verified.ready(43000,357f/16383),"changed panel range invalidates verified adapter");
            try{new DirectBacklight(root.toFile(),"panel0",journal.toFile(),"main","boot",null,null,true);throw new AssertionError("missing driver");}catch(java.io.IOException expected){}
            String dump="DisplayDeviceInfo{uniqueId=\"main\"} mBrightness=[0, 0.2, 1], mBacklight=[0, 0.2, 1] DisplayDeviceInfo{uniqueId=\"rear\"} mBrightness=[0, 1], mBacklight=[0.1, 0.9]";
            check(DirectBacklight.identityMapping(dump,"main"),"verified mapping accepted");
            check(!DirectBacklight.identityMapping(dump,"rear")&&!DirectBacklight.identityMapping(dump,"missing"),"nonidentity or missing panel rejected");
            for(String name:new String[]{"../panel0","/sys/panel0",""})try{new DirectBacklight(root.toFile(),name,journal.toFile(),"main","boot");throw new AssertionError("bad path");}catch(java.io.IOException expected){}
            for(float value:new float[]{Float.NaN,Float.POSITIVE_INFINITY,-.1f,1.1f})try{DirectBacklight.units(value,1000);throw new AssertionError("bad value");}catch(IllegalArgumentException expected){}
            System.out.println("Direct backlight mapping, caps, external writes, driver dimming, recovery and write races passed");
        }finally{try(var paths=Files.walk(root)){for(Path p:paths.sorted(java.util.Comparator.reverseOrder()).toList())Files.delete(p);}}
    }
}
