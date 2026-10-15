// SPDX-License-Identifier: GPL-3.0-only
import org.ref1nd.brightness.LuxFallback;
import org.ref1nd.brightness.PollCadence;
import java.nio.file.Files;
import java.nio.file.Path;

public final class AdapterPolicyTest {
    static void check(boolean b,String msg){if(!b)throw new AssertionError(msg);}
    public static void main(String[] args) throws Exception {
        var cached=new LuxFallback.Sample(42,19000,"sysfs");
        check(LuxFallback.select(0,20,cached,20000,10000).lux==20,"zero front must not hide brighter rear");
        var fusion=LuxFallback.select(.738f,9.774f,cached,20000,10000);
        check(fusion.source.equals("dual_max_v1")&&fusion.lux==9.774f,"lamp behind phone continuously contributes");
        check(LuxFallback.select(20,0,cached,20000,10000).lux==20,"covered rear does not darken front");
        check(LuxFallback.select(0,0,cached,20000,10000).lux==0,"true dual darkness");
        check(LuxFallback.select(20,9,cached,20000,10000).source.equals(fusion.source),"dominant side changes retain learning scope");
        check(LuxFallback.select(9,Float.NaN,cached,20000,10000).source.equals("front"),"invalid rear degrades to front");
        check(LuxFallback.select(9,200001,cached,20000,10000).lux==9,"out-of-range rear never enters fusion");
        var history=new org.ref1nd.brightness.LearningLux();
        history.observe(fusion.lux,20000,1700000020000L);
        check(history.atEvent(1700000023000L,24000,1700000024000L)==9.774f,"events save fused control coordinate");
        var rearReading=new org.ref1nd.brightness.LightReading();rearReading.registration(true,true);rearReading.update(9.774f,10000);
        check(LuxFallback.select(0,rearReading.effective(90000),null,90000,0).lux==9.774f,"stable on-change rear continues to contribute");
        rearReading.registration(false,true);
        check(LuxFallback.select(.7f,rearReading.effective(90001),null,90001,0).source.equals("front"),"unregistered rear cannot reuse old fusion sample");
        check(LuxFallback.select(-1,20,cached,20000,10000).source.equals("rear"),"rear fallback");
        check(LuxFallback.select(-1,-1,cached,20000,10000).lux==42,"file fallback");
        check(LuxFallback.select(-1,-1,cached,35001,10000).lux<0,"stale file rejected");
        check(LuxFallback.select(-1,-1,cached,20000,19500).lux<0,"previous wake rejected");
        check(LuxFallback.parseLux("nan")<0&&LuxFallback.parseLux("Infinity")<0&&LuxFallback.parseLux("1 2")<0,"invalid data");
        String dump="ALS: last 3 events\n\t1 (ts=14.0, wall=13:08:09.415) 10.00, \n\t2 (ts=19.5, wall=13:08:14.915) 0.00, \n\t3 (ts=21.0, wall=13:08:16.415) 999, \nRear ALS: last 1 events\n\t1 (ts=19.9, wall=13:08:15.315) 99, \n";
        var sample=LuxFallback.fromDump(dump,"ALS",20000,15000);
        check(sample!=null&&sample.lux==0&&sample.at==19500&&sample.source.equals("dumpsys:ALS"),"exact sensor, newest nonfuture sample");
        check(LuxFallback.fromDump(dump,"ALS",40000,15000)==null,"stale dump");
        check(LuxFallback.fromDump(dump,"ALS",20000,19800)==null,"dump predates registration");
        check(LuxFallback.fromDump(dump,"Missing",20000,0)==null,"no ambiguous match");
        check(LuxFallback.fromDump(dump,null,20000,15000).lux==0,"discover front without Sensor object");
        check(LuxFallback.fromDump(dump+"Other light: last 1 events\n",null,20000,15000)==null,"ambiguous sensor discovery rejected");
        check(LuxFallback.fromDump(dump+"Ambient Light Raw Data: last 1 events\nOptimization light fov Sensor: last 1 events\n",null,20000,15000).lux==0,"auxiliary channels must not create discovery ambiguity");
        Path root=Files.createTempDirectory("iab-sources");
        try {
            Path sensors=Files.createDirectories(root.resolve("sensors/als"));
            Path rear=Files.createDirectories(root.resolve("sensors/back_als"));
            Path iio=Files.createDirectories(root.resolve("iio/iio:device0"));
            Files.writeString(sensors.resolve("lux"),"12");Files.writeString(rear.resolve("lux"),"1");
            Files.writeString(iio.resolve("in_illuminance_raw"),"20");
            check(LuxFallback.candidates(root.resolve("sensors").toFile(),root.resolve("iio").toFile()).size()==1,"exclude rear and raw unscaled IIO");
            Files.writeString(iio.resolve("in_illuminance_input"),"20");
            check(LuxFallback.candidates(root.resolve("sensors").toFile(),root.resolve("iio").toFile()).size()==2,"ambiguity visible to caller");
        }finally{try(var paths=Files.walk(root)){for(Path p:paths.sorted(java.util.Comparator.reverseOrder()).toList())Files.delete(p);}}
        PollCadence cadence=new PollCadence();
        check(cadence.next(0,10,true,false)==250,"new sample fast");
        check(cadence.next(1000,10,true,false)==800,"stable stage one");
        check(cadence.next(11000,10,true,false)==1600,"stable stage two");
        check(cadence.next(21000,10,true,false)==8000,"long stable sleep");
        check(cadence.next(21001,30,true,false)==250,"light change wakes");
        check(cadence.next(22000,30,false,false)==250,"ramp stays fast");
        check(cadence.next(23000,30,true,true)==2000,"low battery cadence");
        cadence.reset();check(cadence.next(24000,30,true,false)==250,"wake resets cadence");
        System.out.println("Fallback source, freshness, parsing and adaptive cadence passed");
    }
}
