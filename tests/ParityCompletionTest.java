// SPDX-License-Identifier: GPL-3.0-only
import org.ref1nd.brightness.*;
import java.nio.file.*;

public final class ParityCompletionTest {
    static void check(boolean b,String message){if(!b)throw new AssertionError(message);}
    public static void main(String[] args) throws Exception {
        var events=new EventCapability(true);
        check(!events.available()&&events.probe(0),"method existence is not successful access");
        events.success();check(events.available(),"successful query restores health");
        events.failure(1000,"permission denied");
        check(!events.available()&&!events.probe(30999)&&events.probe(31000),"failure degrades with bounded retry");
        events.success();check(events.available(),"transient error recovery");
        check(!new EventCapability(false).probe(999999),"missing method is not retried forever");

        var settings=new SettingsLearning();
        check(!settings.observe(0,false,"1","0"),"default off");
        check(!settings.observe(1000,true,"2","0"),"enable establishes baseline, never learns past changes");
        check(!settings.observe(2999,true,"3","0"),"state settling");
        check(settings.observe(3000,true,"4","0"),"opt-in eligible change");
        check(!settings.observe(4000,false,"5","0"),"protection rejects and resets");
        check(!settings.observe(9000,true,"6","0"),"thermal recovery cannot teach accumulated change");
        settings.reset();check(!settings.observe(10000,true,"7","0"),"user/source switch resets baseline");
        check(settings.observe(12000,true,"7","0.2"),"adjustment-only ROM supported");

        var manual=new ManualLearning();
        manual.begin(5000,105000,10,"event");
        check(!manual.sample(5000,true,.2f),"uncalibrated event waits for output");
        check(!manual.sample(6000,true,.3f),"changing output not learned");
        check(manual.sample(7500,true,.3f)&&manual.value==.3f,"event intent plus settled normalized output");
        check(!manual.sample(7501,false,.1f),"system protection cancels event capture");

        String live="Sensor Device:\n0x00000033) active-count = 1; sampling_period(ms) = {200}\nSensor List:\n"+
            "0x00000033) Exact ALS | Vendor | ver: 1 | type: android.sensor.light(5) | flags: 0x2\n\ton-change | maxDelay=0us\n";
        String data="Exact ALS: last 1 events\n\t1 (ts=20.0, wall=12:00:00.0) 0.0, \n";
        var sample=LuxFallback.fromDump(live+data,"Exact ALS",100000,10000);
        check(sample!=null&&sample.lux==0&&sample.at==20000,"stable on-change accepted with fresh activity evidence");
        check(sample.usable(115000,10000)&&!sample.usable(115001,10000),"verification lease expires when dumps stop");
        check(LuxFallback.fromDump(live+data,"Exact ALS",100000,21000)==null,"previous registration never revived");
        check(LuxFallback.fromDump(live.replace("active-count = 1","active-count = 0")+data,"Exact ALS",100000,10000)==null,"inactive stale sensor rejected");
        check(LuxFallback.fromDump(live.replace("on-change","continuous")+data,"Exact ALS",100000,10000)==null,"continuous still requires new samples");
        check(LuxFallback.fromDump(data,"Exact ALS",100000,10000)==null,"unknown activity cannot extend cache");
        check(LuxFallback.fromDump(live.replace("android.sensor.light(5)","vendor.raw(999)")+data,"Exact ALS",100000,10000)==null,"unknown units rejected");
        check(LuxFallback.fromDump(live+live+data,"Exact ALS",100000,10000)==null,"ambiguous metadata rejected");
        check(LuxFallback.fromDump(live+data,"Exact ALS",19000,10000)==null,"future event rejected despite live subscription");
        check(LuxFallback.fromDump(live+data.replace("0.0,","NaN,"),"Exact ALS",100000,10000)==null,"invalid lux rejected");

        Path root=Files.createTempDirectory("iab-backlight");
        try {
            Path panel=Files.createDirectories(root.resolve("panel"));
            Files.writeString(panel.resolve("brightness"),"200\n");Files.writeString(panel.resolve("actual_brightness"),"120\n");Files.writeString(panel.resolve("max_brightness"),"1000\n");
            var row=Backlight.read(root.toFile()).get(0);
            check(row.requested==200&&row.actual==120&&row.maximum==1000,"requested and driver-reported values remain distinct");
            Files.writeString(panel.resolve("actual_brightness"),"bad");
            check(Backlight.read(root.toFile()).get(0).actual==-1,"unknown must not be reported as zero");
            check(Files.readString(panel.resolve("brightness")).equals("200\n"),"monitor never writes nodes");
        }finally{try(var paths=Files.walk(root)){for(Path p:paths.sorted(java.util.Comparator.reverseOrder()).toList())Files.delete(p);}}
        System.out.println("Runtime event health, opt-in learning, stable fallback leases and read-only backlight passed");
    }
}
