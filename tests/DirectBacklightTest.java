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
            check(!vendor.ready(20000,.048902072f)&&vendor.ready(21000,.048902072f),"bounded compositor compensation with independent driver readback");
            vendor.write(.04f,.4f);driver[0]=655;
            check(vendor.ready(22000,.048902072f),"vendor readback validates raw write despite sysfs zero stub");
            DirectBacklight.restore(root.toFile(),journal.toFile(),"main","boot",true,true,.048902072f,.4f,()->driver[0]);
            check(Files.readString(panel.resolve("brightness")).trim().equals("812"),"unchanged framework restores exact original system node value");
            vendor.released();driver[0]=812;vendor.ready(23000,.048902072f);vendor.ready(24000,.048902072f);vendor.write(.04f,.4f);
            driver[0]=400;check(!vendor.ready(25000,.048902072f),"vendor thermal driver reduction must also yield");
            driver[0]=-1;check(!vendor.ready(30000,.048902072f),"failed vendor readback never authorizes control");
            String dump="DisplayDeviceInfo{uniqueId=\"main\"} mBrightness=[0, 0.2, 1], mBacklight=[0, 0.2, 1] DisplayDeviceInfo{uniqueId=\"rear\"} mBrightness=[0, 1], mBacklight=[0.1, 0.9]";
            check(DirectBacklight.identityMapping(dump,"main"),"verified mapping accepted");
            check(!DirectBacklight.identityMapping(dump,"rear")&&!DirectBacklight.identityMapping(dump,"missing"),"nonidentity or missing panel rejected");
            for(String name:new String[]{"../panel0","/sys/panel0",""})try{new DirectBacklight(root.toFile(),name,journal.toFile(),"main","boot");throw new AssertionError("bad path");}catch(java.io.IOException expected){}
            for(float value:new float[]{Float.NaN,Float.POSITIVE_INFINITY,-.1f,1.1f})try{DirectBacklight.units(value,1000);throw new AssertionError("bad value");}catch(IllegalArgumentException expected){}
            System.out.println("Direct backlight mapping, caps, external writes, driver dimming, recovery and write races passed");
        }finally{try(var paths=Files.walk(root)){for(Path p:paths.sorted(java.util.Comparator.reverseOrder()).toList())Files.delete(p);}}
    }
}
