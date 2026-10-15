// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

import android.content.Context;
import android.content.AttributionSource;
import android.database.ContentObserver;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.hardware.display.DisplayManager;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Build;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.os.UserHandle;
import android.os.UserManager;
import android.provider.Settings;
import android.view.Display;
import org.json.JSONObject;
import java.io.*;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.regex.*;

/** Privileged adapter. The native core owns algorithms and durable state. */
public final class Bridge implements SensorEventListener {
    static final String SOCKET="org.ref1nd.iosbrightness.v7";
    final Handler handler=new Handler(Looper.getMainLooper());
    final Context context;
    final DisplayManager display;
    final Object global;
    final SensorManager sensors;
    final PowerManager power;
    final File module, state;
    final boolean observe;
    final Process core;
    final BufferedWriter coreIn;
    final BufferedReader coreOut;
    final AtomicBoolean closing=new AtomicBoolean();
    final LocalServerSocket server;
    final float[] profileNits, profileBacklight;
    final String displayId;
    JSONObject result=new JSONObject();
    Sensor front, back, proximity;
    float frontLux=-1;
    final LightReading backReading=new LightReading();
    boolean near=false, registered=false, overridden=false, eligible=false;
    int user=-1;
    long userSerial=-1, lastEvent=System.currentTimeMillis(), frontAt=0, pendingAt=0;
    long changedAt=-10000, heartbeat=SystemClock.elapsedRealtime(), lastFrame=0, lastThermal=0, lastStatus=0;
    long started=SystemClock.elapsedRealtime(), lastConfigCheck=0;
    String configText="";
    float temperature=-1, lastWrite=-1;
    int battery=-1;
    boolean charging=false;
    JSONObject pending;
    String eventStatus="waiting", sensorStatus="waiting", previousReason="", bridgeError="", lastEventRejection="";
    int dropped=0;
    List<File> thermalFiles=new ArrayList<>();

    static Object call(Object target,String name,Class<?>[] types,Object...args) throws Exception {
        Method m=target.getClass().getMethod(name,types);m.setAccessible(true);return m.invoke(target,args);
    }
    static float number(Object o,String field) throws Exception {return o.getClass().getField(field).getFloat(o);}
    static int integer(Object o,String field) throws Exception {return o.getClass().getField(field).getInt(o);}
    static String string(Object o,String field) throws Exception {return (String)o.getClass().getField(field).get(o);}
    static long time(Object o,String field) throws Exception {return o.getClass().getField(field).getLong(o);}
    static boolean flag(Object o,String field) throws Exception {return o.getClass().getField(field).getBoolean(o);}
    static Context systemContext() throws Exception {
        Class<?> at=Class.forName("android.app.ActivityThread");
        Object thread=at.getMethod("systemMain").invoke(null);
        return (Context)at.getMethod("getSystemContext").invoke(thread);
    }
    static void release(DisplayManager dm) throws Exception {call(dm,"setTemporaryBrightness",new Class<?>[]{int.class,float.class},0,Float.NaN);}
    static String boundedLine(Reader r,int limit) throws IOException {
        StringBuilder b=new StringBuilder();int c;
        while((c=r.read())!=-1&&c!='\n') {if(b.length()>=limit)throw new IOException("line too large");b.append((char)c);}
        return c==-1&&b.length()==0?null:b.toString();
    }
    static String read(File f,int max) throws IOException {
        try(InputStream in=new FileInputStream(f)) {byte[] b=new byte[max+1];int n=0,k;while(n<b.length&&(k=in.read(b,n,b.length-n))>0)n+=k;if(n>max)throw new IOException("file too large");return new String(b,0,n,StandardCharsets.UTF_8);}
    }
    static void replace(File path,String text) throws IOException {
        File tmp=File.createTempFile(".status-",".tmp",path.getParentFile());
        try {try(FileOutputStream out=new FileOutputStream(tmp)){out.write(text.getBytes(StandardCharsets.UTF_8));}
            Files.move(tmp.toPath(),path.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
        }finally{tmp.delete();}
    }
    JSONObject request(String cmd) throws Exception {
        coreIn.write(cmd);coreIn.newLine();coreIn.flush();
        String line=boundedLine(coreOut,131072);if(line==null)throw new IOException("native core exited");
        JSONObject r=new JSONObject(line);
        if(!r.optBoolean("ok"))throw new IOException(r.optString("error","core request failed"));
        return r;
    }
    Bridge(Context c,File mod,File dir,boolean observation) throws Exception {
        context=c;module=mod;state=dir;observe=observation;
        if(!dir.isDirectory())throw new IOException("state directory not prepared");
        display=(DisplayManager)c.getSystemService(Context.DISPLAY_SERVICE);
        global=Class.forName("android.hardware.display.DisplayManagerGlobal").getMethod("getInstance").invoke(null);
        displayId=(String)call(display.getDisplay(0),"getUniqueId",new Class<?>[]{});
        Object info=call(global,"getBrightnessInfo",new Class<?>[]{int.class},0);
        for(String f:new String[]{"brightness","adjustedBrightness","brightnessMinimum","brightnessMaximum"})number(info,f);
        flag(info,"isBrightnessOverrideByWindow");
        // Require the actual target-ROM interfaces before starting a controller.
        display.getClass().getMethod("setTemporaryBrightness",int.class,float.class);
        display.getClass().getMethod("getBrightnessEvents");
        Scanner scan=new Scanner(read(new File(module,"profiles/altdb.profile"),8192));scan.useLocale(Locale.ROOT);
        if(!scan.next().equals("IAB_PROFILE_V1"))throw new IOException("profile version");scan.next();int n=scan.nextInt();
        if(n<2||n>256)throw new IOException("profile length");
        profileNits=new float[n];profileBacklight=new float[n];for(int i=0;i<n;i++){profileNits[i]=scan.nextFloat();profileBacklight[i]=scan.nextFloat();}scan.close();
        validatePanel();
        sensors=(SensorManager)c.getSystemService(Context.SENSOR_SERVICE);
        power=(PowerManager)c.getSystemService(Context.POWER_SERVICE);
        front=sensors.getDefaultSensor(Sensor.TYPE_LIGHT);proximity=sensors.getDefaultSensor(Sensor.TYPE_PROXIMITY);
        for(Sensor s:sensors.getSensorList(Sensor.TYPE_ALL))if(s.getStringType().contains("back_lux")&&!s.isWakeUpSensor()){back=s;break;}
        if(front==null)throw new IOException("No front light sensor");
        // Binding the abstract socket is also the bridge singleton guard.
        server=new LocalServerSocket(SOCKET);
        Process launched=null;
        try {
            launched=new ProcessBuilder(new File(module,"bin/ios_brightness_daemon").getPath(),state.getPath(),new File(module,"profiles/altdb.profile").getPath()).redirectError(ProcessBuilder.Redirect.INHERIT).start();
            core=launched;coreIn=new BufferedWriter(new OutputStreamWriter(core.getOutputStream(),StandardCharsets.UTF_8));coreOut=new BufferedReader(new InputStreamReader(core.getInputStream(),StandardCharsets.UTF_8));
        }catch(Exception e){server.close();if(launched!=null)launched.destroy();throw e;}
    }
    void validatePanel() throws Exception {
        Display.Mode mode=display.getDisplay(0).getMode();
        if(mode.getPhysicalWidth()!=1200||mode.getPhysicalHeight()!=2608)throw new IOException("Unsupported main-panel geometry");
        Process dump=new ProcessBuilder("/system/bin/dumpsys","display").redirectErrorStream(true).start();
        Thread timeout=new Thread(()->{SystemClock.sleep(5000);if(dump.isAlive())dump.destroyForcibly();},"iab-probe-timeout");timeout.setDaemon(true);timeout.start();
        StringBuilder body=new StringBuilder();
        try(Reader reader=new InputStreamReader(dump.getInputStream(),StandardCharsets.UTF_8)) {
            char[] b=new char[4096];int n;
            while((n=reader.read(b))>0){body.append(b,0,n);if(body.length()>2000000){dump.destroyForcibly();throw new IOException("display dump too large");}}
        }
        if(dump.waitFor()!=0)throw new IOException("display calibration probe failed");
        String text=body.toString();
        int start=text.indexOf("uniqueId=\""+displayId+"\"");
        if(start<0)throw new IOException("main display calibration identity missing");
        int end=text.indexOf("DisplayDeviceInfo{",start);
        String segment=text.substring(start,end<0?text.length():end);
        Matcher m=Pattern.compile("mBacklight=\\[([^\\]]+)\\], mNits=\\[([^\\]]+)\\]").matcher(segment);
        if(!m.find())throw new IOException("main panel calibration unavailable");
        String[] bs=m.group(1).split(","), ns=m.group(2).split(",");
        if(bs.length!=profileBacklight.length||ns.length!=profileNits.length)throw new IOException("panel calibration changed");
        for(int i=0;i<bs.length;i++) {
            float b=Float.parseFloat(bs[i].trim()),n=Float.parseFloat(ns[i].trim());
            if(!Float.isFinite(b)||!Float.isFinite(n)||Math.abs(b-profileBacklight[i])>.00001||Math.abs(n-profileNits[i])>.05)throw new IOException("panel calibration mismatch");
        }
    }
    float toBacklight(float nits) {
        if(nits<=profileNits[0])return profileBacklight[0];
        for(int i=1;i<profileNits.length;i++)if(nits<=profileNits[i])return profileBacklight[i-1]+(nits-profileNits[i-1])/(profileNits[i]-profileNits[i-1])*(profileBacklight[i]-profileBacklight[i-1]);
        return profileBacklight[profileBacklight.length-1];
    }
    void begin() throws Exception {
        Runtime.getRuntime().addShutdownHook(new Thread(this::shutdown,"iab-shutdown"));
        ContentObserver observer=new ContentObserver(handler) {
            @Override public void onChange(boolean selfChange) {
                if(closing.get())return;
                changedAt=SystemClock.elapsedRealtime();
                try{request("HOLD "+changedAt);clearOverride();}catch(Exception e){bridgeError=e.toString();}
            }
        };
        Method register=context.getContentResolver().getClass().getMethod("registerContentObserver",android.net.Uri.class,boolean.class,ContentObserver.class,int.class);
        for(String key:new String[]{Settings.System.SCREEN_BRIGHTNESS,Settings.System.SCREEN_BRIGHTNESS_MODE,"screen_auto_brightness_adj"})register.invoke(context.getContentResolver(),Settings.System.getUriFor(key),false,observer,-1);
        new Thread(this::serve,"iab-control").start();
        Thread watchdog=new Thread(()->{
            while(!closing.get()) {
                SystemClock.sleep(1000);
                if(SystemClock.elapsedRealtime()-heartbeat>5000) {
                    System.err.println("Bridge heartbeat expired; releasing display");
                    try { if(!observe)release(display); } catch(Exception ignored) {}
                    core.destroy();System.exit(1);
                }
            }
        },"iab-watchdog");watchdog.setDaemon(true);watchdog.start();
        handler.post(this::tick);
    }
    void clearOverride() throws Exception {
        if(overridden&&!observe){release(display);overridden=false;lastWrite=-1;}
    }
    void registerSensors(boolean wanted) {
        if(wanted==registered)return;
        if(wanted) {
            registered=sensors.registerListener(this,front,250000,handler);
            boolean backRegistered=registered&&back!=null&&sensors.registerListener(this,back,500000,handler);
            backReading.registration(backRegistered,back!=null&&back.getReportingMode()==Sensor.REPORTING_MODE_ON_CHANGE);
            if(registered&&proximity!=null)sensors.registerListener(this,proximity,SensorManager.SENSOR_DELAY_NORMAL,handler);
            sensorStatus=registered?front.getName():"registration_failed";
        }else{sensors.unregisterListener(this);registered=false;frontLux=-1;backReading.registration(false,false);frontAt=0;near=false;sensorStatus="suspended";}
    }
    @Override public void onSensorChanged(SensorEvent e) {
        float v=e.values[0];if(!Float.isFinite(v)||v<0)return;
        if(e.sensor==front){frontLux=v;frontAt=SystemClock.elapsedRealtime();}
        else if(e.sensor==back){backReading.update(v,SystemClock.elapsedRealtime());}
        else if(e.sensor==proximity)near=v<Math.min(5,proximity.getMaximumRange());
    }
    @Override public void onAccuracyChanged(Sensor s,int accuracy) {}
    int activeUser() throws Exception {return (Integer)Class.forName("android.app.ActivityManager").getMethod("getCurrentUser").invoke(null);}
    long serial(int id) throws Exception {
        UserHandle handle=(UserHandle)UserHandle.class.getMethod("of",int.class).invoke(null,id);
        return ((UserManager)context.getSystemService(Context.USER_SERVICE)).getSerialNumberForUser(handle);
    }
    int mode(int uid) throws Exception {
        // app_process is not an AMS-registered application. Use the external
        // provider API for privileged clients; never parse settings XML/ABX.
        Object am=Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
        IBinder token=new Binder();
        Object holder=call(am,"getContentProviderExternal",new Class<?>[]{String.class,int.class,IBinder.class,String.class},"settings",0,token,"ios-auto-brightness");
        if(holder==null)throw new IOException("settings provider unavailable");
        try {
            Object provider=holder.getClass().getField("provider").get(holder);
            Bundle extras=new Bundle();extras.putInt("_user",uid);
            AttributionSource source=new AttributionSource.Builder(android.os.Process.myUid()).build();
            Bundle values=(Bundle)call(provider,"call",new Class<?>[]{AttributionSource.class,String.class,String.class,String.class,Bundle.class},source,"settings","GET_system",Settings.System.SCREEN_BRIGHTNESS_MODE,extras);
            if(values==null)return -1;
            String value=values.getString("value");
            return value==null?-1:Integer.parseInt(value);
        }finally{call(am,"removeContentProviderExternalAsUser",new Class<?>[]{String.class,IBinder.class,int.class},"settings",token,0);}
    }
    void thermal(long now) {
        if(now-lastThermal<2000)return;lastThermal=now;
        try{battery=Integer.parseInt(read(new File("/sys/class/power_supply/battery/capacity"),32).trim());}catch(Exception e){battery=-1;}
        try{charging=read(new File("/sys/class/power_supply/battery/status"),32).trim().equals("Charging");}catch(Exception e){charging=false;}
        if(thermalFiles.isEmpty()) {
            File[] zones=new File("/sys/class/thermal").listFiles();
            if(zones!=null)for(File z:zones)if(z.getName().startsWith("thermal_zone"))try{
                String type=read(new File(z,"type"),256).trim().toLowerCase(Locale.ROOT);
                // Skin/battery only; CPU temperatures have different operating limits.
                if(type.contains("skin")||type.equals("battery")||type.contains("shell"))thermalFiles.add(new File(z,"temp"));
            }catch(Exception ignored){}
        }
        temperature=-1;
        for(File f:thermalFiles)try{float t=Float.parseFloat(read(f,32).trim());if(t>1000)t/=1000;if(Float.isFinite(t)&&t>=0&&t<=150)temperature=Math.max(temperature,t);}catch(Exception ignored){}
    }
    void events(long now,boolean canLearn) {
        try {
            List<?> list=(List<?>)call(display,"getBrightnessEvents",new Class<?>[]{});
            long since=lastEvent,max=lastEvent;Object newest=null;
            for(Object e:list) {
                long ts=time(e,"timeStamp");max=Math.max(max,ts);
                if(ts<=since)continue;
                float[] lux=(float[])e.getClass().getField("luxValues").get(e);
                long[] samples=(long[])e.getClass().getField("luxTimestamps").get(e);
                String rejection=EventGate.rejection(ts,since,System.currentTimeMillis(),integer(e,"userId"),user,string(e,"uniqueDisplayId"),displayId,lux,samples,number(e,"brightness"),number(e,"powerBrightnessFactor"),flag(e,"reduceBrightColors"),canLearn);
                if(rejection.isEmpty()) {
                    if(newest==null||ts>time(newest,"timeStamp"))newest=e;
                }else {dropped++;lastEventRejection=rejection;}
            }
            lastEvent=max;
            if(newest!=null) {
                float[] lux=(float[])newest.getClass().getField("luxValues").get(newest);
                pending=new JSONObject().put("time",time(newest,"timeStamp")).put("lux",lux[lux.length-1]).put("value",toBacklight(number(newest,"brightness")));
                pendingAt=now;eventStatus="pending";request("HOLD "+now);clearOverride();
            }
            if(pending!=null&&now-pendingAt>=1500) {
                if(canLearn) {
                    JSONObject learned=request("EVENT "+pending.getLong("time")+" "+pending.getDouble("lux")+" "+pending.getDouble("value"));
                    eventStatus=learned.has("accepted")?"rejected_unstable":"saved";
                }else eventStatus="discarded_state_changed";
                pending=null;
            }
        }catch(Exception e){
            eventStatus="error: "+rootCause(e);
            if(pending!=null&&now-pendingAt>10000)pending=null;
        }
    }
    static String rootCause(Throwable e){while(e.getCause()!=null)e=e.getCause();return e.toString();}
    void tick() {
        if(closing.get())return;
        long now=SystemClock.elapsedRealtime();heartbeat=now;
        try {
            int currentUser=activeUser();long currentSerial=serial(currentUser);
            if(currentSerial<0)throw new IOException("user serial unavailable");
            if(currentUser!=user||currentSerial!=userSerial) {
                clearOverride();user=currentUser;userSerial=currentSerial;pending=null;lastEvent=System.currentTimeMillis();
                result=request("USER "+userSerial);
                File legacy=new File(state,"legacy-offset.dat");
                if(user==0&&result.optString("storage").equals("new")&&legacy.isFile())result=request("MIGRATE "+JSONObject.quote(legacy.getPath()));
            }
            boolean automatic=mode(user)==1;
            // HyperOS can report STATE_ON while an AOD pulse is still dozing.
            Display main=display.getDisplay(0);boolean on=main!=null&&main.getState()==Display.STATE_ON&&power.isInteractive();
            registerSensors(automatic&&on);
            if(registered&&front.getReportingMode()!=Sensor.REPORTING_MODE_ON_CHANGE&&frontAt>0&&now-frontAt>30000) {
                // A continuous sensor must keep reporting. Silence from an
                // on-change sensor means the value is unchanged, not stale.
                registerSensors(false);registerSensors(automatic&&on);frontAt=0;
            }
            if(now-lastConfigCheck>=2000) {
                lastConfigCheck=now;
                String fresh=read(new File(state,"config.conf"),32768);
                if(!fresh.equals(configText)) {
                    configText=fresh;
                    try{request("RELOAD");}catch(Exception e){System.err.println("config: "+rootCause(e));}
                }
            }
            Object info=call(global,"getBrightnessInfo",new Class<?>[]{int.class},0);
            if(info==null)throw new IOException("brightness limits unavailable");
            float minimum=number(info,"brightnessMinimum"),maximum=number(info,"brightnessMaximum"),current=number(info,"adjustedBrightness");
            boolean blocked=flag(info,"isBrightnessOverrideByWindow")||integer(info,"highBrightnessMode")==2;
            thermal(now);
            // On-change light sensors may be silent while stable; registration stays active.
            float lux=registered&&frontAt>=started?frontLux:-1;
            float backLux=backReading.effective(now);
            eligible=automatic&&on&&!blocked&&!near&&lux>=0;
            result=request(String.format(Locale.ROOT,"FRAME %d %d %d %.8f %.8f %d %.4f %d %d %.9f %.9f %.9f %d",now,automatic?1:0,on?1:0,lux,backLux,near?1:0,temperature,battery,charging?1:0,current,minimum,maximum,blocked?1:0));
            if(!automatic||!on){pending=null;lastEvent=System.currentTimeMillis();}
            else if(now-lastFrame>=750){events(now,eligible);lastFrame=now;result=request("STATUS");}
            if(result.optBoolean("apply")&&now-changedAt>=2000&&pending==null&&!observe) {
                float target=(float)result.getDouble("target");
                if(!overridden||Math.abs(target-lastWrite)>.00001) {
                    call(display,"setTemporaryBrightness",new Class<?>[]{int.class,float.class},0,target);
                    lastWrite=target;overridden=true;
                }
            }else clearOverride();
            bridgeError="";
            result.put("observation_only",observe).put("bridge","active").put("event_status",eventStatus).put("events_dropped",dropped)
                .put("last_event_rejection",lastEventRejection)
                .put("sensor",sensorStatus).put("back_sensor",back==null?"unavailable":back.getName()).put("proximity_sensor",proximity==null?"unavailable":proximity.getName())
                .put("front_lux",frontLux).put("back_lux",backReading.raw()).put("back_lux_effective",backLux)
                .put("back_sample_age_ms",backReading.age(now)).put("back_registered",backReading.isRegistered())
                .put("back_reporting_mode",back==null?-1:back.getReportingMode())
                .put("temperature",temperature).put("battery",battery).put("mode",automatic?"auto":"manual_or_unknown").put("screen_on",on)
                .put("current",current).put("setting",number(info,"brightness")).put("minimum",minimum).put("maximum",maximum).put("display",0).put("uptime_ms",now-started).put("heartbeat_ms",now).put("cpu_percent",100.0*android.os.Process.getElapsedCpuTime()/Math.max(1,now-started));
            String reason=result.optString("reason");
            if(!reason.equals(previousReason)){System.out.println("state="+reason+" storage="+result.optString("storage"));previousReason=reason;}
            if(now-lastStatus>=1000){replace(new File(state,"status.json"),result.toString());lastStatus=now;}
        }catch(Exception e) {
            String error=rootCause(e);if(!error.equals(bridgeError))System.err.println("bridge: "+error);bridgeError=error;
            try{clearOverride();}catch(Exception ignored){}
            try{result=new JSONObject().put("ok",false).put("bridge","degraded").put("error",bridgeError).put("heartbeat_ms",now);replace(new File(state,"status.json"),result.toString());}catch(Exception ignored){}
            if(!core.isAlive()) {shutdown();System.exit(1);}
        }finally{heartbeat=SystemClock.elapsedRealtime();if(!closing.get())handler.postDelayed(this::tick,registered?250:1000);}
    }
    JSONObject control(String line) throws Exception {
        String[] parts=line.split(" ",2);String cmd=parts[0];
        if(cmd.equals("status"))return new JSONObject(result.toString());
        if(cmd.equals("config-get"))return request("CONFIG_GET");
        if(cmd.equals("learn-export"))return request("EXPORT");
        if(cmd.equals("learn-clear")){pending=null;return request("CLEAR");}
        if(cmd.equals("stop")){handler.postDelayed(()->{shutdown();System.exit(0);},200);return new JSONObject().put("ok",true);}
        if(cmd.equals("config-set")||cmd.equals("learn-import")) {
            if(parts.length!=2)throw new IOException("payload required");
            byte[] decoded=android.util.Base64.decode(parts[1],android.util.Base64.DEFAULT);
            if(decoded.length>32768)throw new IOException("payload too large");
            File temp=File.createTempFile(".request-",".tmp",state);
            try {try(FileOutputStream out=new FileOutputStream(temp)){out.write(decoded);out.getFD().sync();}
                JSONObject reply=request((cmd.equals("config-set")?"CONFIG ":"IMPORT ")+JSONObject.quote(temp.getPath()));
                pending=null;return reply;
            }finally{temp.delete();}
        }
        throw new IOException("unsupported command");
    }
    void serve() {
        while(!closing.get())try(LocalSocket socket=server.accept()) {
            socket.setSoTimeout(3000);
            if(socket.getPeerCredentials().getUid()!=0)continue;
            String line=boundedLine(new InputStreamReader(socket.getInputStream(),StandardCharsets.UTF_8),50000);
            if(line==null)continue;
            JSONObject reply;
            try{FutureTask<JSONObject> f=new FutureTask<>(()->control(line));handler.post(f);reply=f.get(4,TimeUnit.SECONDS);}
            catch(Exception e){reply=new JSONObject().put("ok",false).put("error",rootCause(e));}
            socket.getOutputStream().write((reply+"\n").getBytes(StandardCharsets.UTF_8));
        }catch(Exception e){if(!closing.get())System.err.println("control: "+rootCause(e));}
    }
    void shutdown() {
        if(!closing.compareAndSet(false,true))return;
        sensors.unregisterListener(this);
        // Release even if the last call completed before overridden was set.
        try{if(!observe)release(display);}catch(Exception e){System.err.println("release: "+e);}
        try{server.close();coreIn.close();}catch(Exception ignored){}
        core.destroy();
        try{replace(new File(state,"status.json"),new JSONObject().put("ok",true).put("bridge","stopped").toString());}catch(Exception ignored){}
    }
    public static void main(String[] args) {
        try { run(args); }
        catch(Throwable e) {System.err.println("iOS Auto Brightness: "+rootCause(e));System.exit(1);}
    }
    static void run(String[] args) throws Exception {
        if(args.length>0&&args[0].equals("ctl")) {
            if(args.length<2)throw new IllegalArgumentException("ctl command required");
            try(LocalSocket socket=new LocalSocket()) {
                socket.connect(new LocalSocketAddress(SOCKET));socket.setSoTimeout(6000);
                String cmd=String.join(" ",Arrays.copyOfRange(args,1,args.length));
                socket.getOutputStream().write((cmd+"\n").getBytes(StandardCharsets.UTF_8));
                String reply=boundedLine(new InputStreamReader(socket.getInputStream(),StandardCharsets.UTF_8),131072);
                if(reply==null)throw new IOException("no reply");System.out.println(reply);
                if(!new JSONObject(reply).optBoolean("ok"))System.exit(1);
            }return;
        }
        Looper.prepareMainLooper();Context context=systemContext();
        if(args.length==1&&args[0].equals("release")){release((DisplayManager)context.getSystemService(Context.DISPLAY_SERVICE));return;}
        if(args.length<2||args.length>3)throw new IllegalArgumentException("MODULE STATE [--observe]");
        Bridge bridge=new Bridge(context,new File(args[0]),new File(args[1]),args.length==3&&args[2].equals("--observe"));
        bridge.begin();Looper.loop();
    }
}
