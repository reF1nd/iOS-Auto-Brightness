// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

import android.content.Context;
import android.content.BroadcastReceiver;
import android.content.Intent;
import android.content.IntentFilter;
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
import android.os.BatteryManager;
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
    final String version;
    final String sourceCommit;
    final boolean calibrated;
    final EventCapability eventCapability;
    final SettingsLearning settingsLearning=new SettingsLearning();
    String calibrationStatus;
    final ProtectionPolicy protection=new ProtectionPolicy();
    final InteractionGuard interaction=new InteractionGuard();
    boolean interactionProbeBusy=false;
    long interactionProbeAt=-1000;
    String brightnessListener="polling_fallback";
    String protectionReason="", learningSource="", learningScope="", sensorConfigText="", sensorSelectionStatus="auto";
    SensorChoice sensorChoice=new SensorChoice("");
    final String bootId;
    final ManualLearning manual=new ManualLearning();
    String manualSlider=null, manualAdjustment=null;
    int thermalStatus=-1, brightnessMaxReason=-1;
    DirectBacklight direct;
    boolean frameBlocked=true;
    final boolean referenceControl=true;
    boolean referenceLearningAvailable=false;
    String outputStatus="backlight_unavailable";
    final OutputProbeGuard outputProbe=new OutputProbeGuard();
    boolean probeSession=false, outputLearningSafe=false, observationLearningSafe=false, outputSliderActive=false;
    long referenceAwakeAt=-1, referenceTickAt=-10000;
    volatile String outputFault="";
    final ReferenceSliderSampling sliderSampling=new ReferenceSliderSampling();
    JSONObject result=new JSONObject();
    Sensor front, back, proximity;
    float frontLux=-1;
    final LightReading backReading=new LightReading(), frontReading=new LightReading();
    final LearningLux learningLux=new LearningLux();
    final PollCadence cadence=new PollCadence();
    final Runnable tickTask=this::tick;
    final AtomicBoolean fallbackBusy=new AtomicBoolean();
    LuxFallback.Sample fallbackSample;
    long sensorSession=0, lastSensorRetry=0, lastFallbackRequest=-10000;
    volatile long heartbeat=SystemClock.elapsedRealtime(), nextTickAt=0;
    int pollMs=250;
    boolean wakeQueued=false;
    String luxSource="unavailable", fallbackStatus="idle";
    boolean near=false, registered=false, overridden=false, eligible=false;
    int user=-1;
    long userSerial=-1, lastEvent=System.currentTimeMillis(), frontAt=0, pendingAt=0;
    long changedAt=-10000, lastFrame=0, lastThermal=0, lastStatus=0;
    long started=SystemClock.elapsedRealtime(), lastConfigCheck=0;
    final long cpuStarted=android.os.Process.getElapsedCpuTime();
    String configText="";
    float temperature=-1, lastWrite=-1, lastLoggedTarget=-1;
    long lastDecisionLog=0;
    int battery=-1;
    boolean charging=false, frameworkCharging=false;
    int frameworkBattery=-1;
    float batteryTemperature=-1;
    JSONObject pending;
    float lastLearningLux=-1, lastFrameworkLux=-1;
    String eventStatus="waiting", sensorStatus="waiting", previousReason="", bridgeError="", lastEventRejection="";
    int dropped=0, windowState=-1;
    long windowAt=-10000, windowRequested=-10000;
    boolean windowBusy=false;
    List<File> thermalFiles=new ArrayList<>();

    static Object call(Object target,String name,Class<?>[] types,Object...args) throws Exception {
        Method m=target.getClass().getMethod(name,types);m.setAccessible(true);return m.invoke(target,args);
    }
    static float number(Object o,String field) throws Exception {return o.getClass().getField(field).getFloat(o);}
    static int integer(Object o,String field) throws Exception {return o.getClass().getField(field).getInt(o);}
    static String string(Object o,String field) throws Exception {return (String)o.getClass().getField(field).get(o);}
    static long time(Object o,String field) throws Exception {return o.getClass().getField(field).getLong(o);}
    static boolean flag(Object o,String field) throws Exception {return o.getClass().getField(field).getBoolean(o);}
    static boolean hasField(Object o,String field){try{o.getClass().getField(field);return true;}catch(Exception e){return false;}}
    static Context systemContext() throws Exception {
        Class<?> at=Class.forName("android.app.ActivityThread");
        Object thread=at.getMethod("systemMain").invoke(null);
        return (Context)at.getMethod("getSystemContext").invoke(thread);
    }
    static final File BACKLIGHT_ROOT=new File("/sys/class/backlight");
    static void release(Context context,File state,File module) throws Exception {
        // Restore permissions even if the display disappeared or framework IPC fails.
        String boot=read(new File("/proc/sys/kernel/random/boot_id"),128).trim();
        DirectBacklight.unlock(BACKLIGHT_ROOT,new File(state,"direct-output.properties"),boot);
        DisplayManager dm=(DisplayManager)context.getSystemService(Context.DISPLAY_SERVICE);
        Object global=Class.forName("android.hardware.display.DisplayManagerGlobal").getMethod("getInstance").invoke(null);
        Display main=dm.getDisplay(0);if(main==null)return;
        Object info=call(global,"getBrightnessInfo",new Class<?>[]{int.class},0);
        PowerManager pm=(PowerManager)context.getSystemService(Context.POWER_SERVICE);
        boolean safe=pm.getCurrentThermalStatus()==0&&hasField(info,"brightnessMaxReason")&&integer(info,"brightnessMaxReason")==0;
        boolean manual=false;
        try{manual=mode(activeUser())==0;}catch(Exception ignored){} // Unknown mode cannot authorize raising brightness.
        DirectBacklight.restore(BACKLIGHT_ROOT,new File(state,"direct-output.properties"),
            (String)call(main,"getUniqueId",new Class<?>[]{}),read(new File("/proc/sys/kernel/random/boot_id"),128).trim(),
            main.getState()==Display.STATE_ON&&pm.isInteractive(),safe&&manual,
            number(info,hasField(info,"adjustedBrightness")?"adjustedBrightness":"brightness"),number(info,"brightnessMaximum"),()->standalonePanelRead(module));
    }
    static long standalonePanelRead(File module) throws IOException {
        Process p=new ProcessBuilder(new File(module,"bin/ios_brightness_daemon").getPath(),"--panel-read").start();
        try {
            if(!p.waitFor(2,TimeUnit.SECONDS))throw new IOException("panel readback timeout");
            JSONObject result=new JSONObject(boundedLine(new InputStreamReader(p.getInputStream(),StandardCharsets.UTF_8),4096));
            if(!result.optBoolean("ok"))throw new IOException("panel readback unavailable");
            return result.getLong("driver_brightness");
        }catch(Exception e){throw new IOException("panel readback failed",e);}finally{p.destroy();}
    }
    long panelRead() throws IOException {
        try{return request("PANEL_READ").getLong("driver_brightness");}catch(Exception e){throw new IOException("panel readback failed",e);}
    }
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
        bootId=read(new File("/proc/sys/kernel/random/boot_id"),128).trim();
        version=read(new File(module,"module.prop"),4096).replaceAll("(?s).*?\\nversion=([^\\n]+).*", "$1");
        sourceCommit=Health.sourceCommit(module);
        if(!dir.isDirectory())throw new IOException("state directory not prepared");
        File fault=new File(state,"output-fault");
        if(!observe&&fault.isFile()){outputFault=read(fault,4096).trim();if(outputFault.isEmpty())outputFault="output_fault_record";}
        display=(DisplayManager)c.getSystemService(Context.DISPLAY_SERVICE);
        global=Class.forName("android.hardware.display.DisplayManagerGlobal").getMethod("getInstance").invoke(null);
        displayId=(String)call(display.getDisplay(0),"getUniqueId",new Class<?>[]{});
        Object info=call(global,"getBrightnessInfo",new Class<?>[]{int.class},0);
        for(String f:new String[]{"brightness","brightnessMinimum","brightnessMaximum"})number(info,f);
        boolean events=true;
        try{display.getClass().getMethod("getBrightnessEvents");}catch(NoSuchMethodException e){events=false;}
        PanelProfile profile;String calibrationDump="";
        try{calibrationDump=displayDump();profile=PanelProfile.fromDump(calibrationDump,displayId);calibrationStatus="system_calibration";}
        catch(Exception e){profile=PanelProfile.generic(displayId);calibrationStatus="generic_normalized_learning: "+rootCause(e);}
        calibrated=profile.calibrated;eventCapability=new EventCapability(events);
        profileNits=profile.nits;profileBacklight=profile.brightness;
        String profileName=profile.id;
        // Preserve existing learning only when the complete measured mapping is identical.
        if(calibrated&&profile.matches(read(new File(module,"profiles/altdb.profile"),8192)))profileName="altdb-main-hyperos4";
        replace(new File(state,"active.profile"),profile.encode(profileName));
        sensors=(SensorManager)c.getSystemService(Context.SENSOR_SERVICE);
        power=(PowerManager)c.getSystemService(Context.POWER_SERVICE);
        try {
            if(!calibrated||!DirectBacklight.identityMapping(calibrationDump,displayId))throw new IOException("verified identity brightness/backlight mapping required");
            String node="";boolean verifiedPanel=false;
            File binding=new File(state,"backlight.conf");
            if(binding.isFile()) {
                Properties choice=new Properties();choice.load(new StringReader(read(binding,4096)));
                if(choice.size()!=2||!displayId.equals(choice.getProperty("display")))throw new IOException("backlight binding display mismatch");
                node=choice.getProperty("node","");
            }else {
                // Product names can be spoofed per process. Use the verified panel
                // calibration, main display geometry and dual-panel node ranges.
                Display.Mode mode=display.getDisplay(0).getMode();
                if(profileName.equals("altdb-main-hyperos4")&&mode.getPhysicalWidth()==1200&&mode.getPhysicalHeight()==2608
                    &&Backlight.number(new File(BACKLIGHT_ROOT,"panel0-backlight/max_brightness"))==16383
                    &&Backlight.number(new File(BACKLIGHT_ROOT,"panel1-backlight/max_brightness"))==4095){node="panel0-backlight";verifiedPanel=true;}
                else throw new IOException("explicit main-panel backlight.conf binding required");
            }
            direct=new DirectBacklight(BACKLIGHT_ROOT,node,new File(state,"direct-output.properties"),displayId,bootId,verifiedPanel?this::panelRead:null,BacklightLease.POSIX,verifiedPanel);
            direct.referenceMode();outputStatus="verifying_mapping";
        }catch(Exception e){outputStatus="backlight_unavailable: "+rootCause(e);}
        loadSensorChoice();
        // Missing or failed primary sensors can use the independently registered rear/fallback source.
        // Binding the abstract socket is also the bridge singleton guard.
        server=new LocalServerSocket(SOCKET);
        Process launched=null;
        try {
            if(!observe)release(context,state,module);
            launched=new ProcessBuilder(new File(module,"bin/ios_brightness_daemon").getPath(),state.getPath(),new File(state,"active.profile").getPath()).redirectError(ProcessBuilder.Redirect.INHERIT).start();
            core=launched;coreIn=new BufferedWriter(new OutputStreamWriter(core.getOutputStream(),StandardCharsets.UTF_8));coreOut=new BufferedReader(new InputStreamReader(core.getInputStream(),StandardCharsets.UTF_8));
            request("POLICY v68");
        }catch(Exception e){server.close();if(launched!=null)launched.destroy();throw e;}
    }
    static Sensor exactSensor(SensorManager manager,String name) throws IOException {
        Sensor found=null;
        for(Sensor s:manager.getSensorList(Sensor.TYPE_ALL))if(s.getName().equals(name)) {
            if(found!=null)throw new IOException("ambiguous sensor name: "+name);found=s;
        }
        if(found==null)throw new IOException("sensor not found: "+name);
        if(!SensorChoice.luxSensor(found.getType(),found.getName(),found.getStringType()))throw new IOException("not a verified lux sensor");
        return found;
    }
    static void validateChoice(SensorManager manager,SensorChoice choice) throws Exception {
        if(!choice.front.isEmpty())exactSensor(manager,choice.front);
        if(!choice.back.isEmpty()&&!choice.back.equals("none")) {
            Sensor rear=exactSensor(manager,choice.back);
            if(choice.front.isEmpty()&&rear==manager.getDefaultSensor(Sensor.TYPE_LIGHT))throw new IOException("specify a different front sensor before using the default as rear");
        }
        if(!choice.sysfs.isEmpty()&&!LuxFallback.valid(LuxFallback.parseLux(read(new File(choice.sysfs),128))))throw new IOException("selected node does not provide valid lux");
        if(!choice.front.isEmpty()&&choice.front.equals(choice.back))throw new IOException("front and rear cannot be the same sensor");
    }
    void loadSensorChoice() throws Exception {
        File path=new File(state,"sensors.conf");String text=path.isFile()?read(path,4096):"";
        SensorChoice choice=new SensorChoice(text);validateChoice(sensors,choice);
        Sensor selected=sensors.getDefaultSensor(Sensor.TYPE_LIGHT);
        sensorSelectionStatus="auto";
        if(!choice.front.isEmpty()){selected=exactSensor(sensors,choice.front);sensorSelectionStatus="explicit";}
        else {
            String preferred=(String)Class.forName("android.os.SystemProperties").getMethod("get",String.class,String.class).invoke(null,"persist.sys.display.als_name","");
            if(!preferred.isEmpty())try{selected=exactSensor(sensors,preferred);sensorSelectionStatus="system_property";}catch(IOException e){sensorSelectionStatus="property_unmatched_using_default";}
        }
        Sensor rear=null;
        if(!choice.back.isEmpty()&&!choice.back.equals("none"))rear=exactSensor(sensors,choice.back);
        else if(choice.back.isEmpty())for(Sensor sensor:sensors.getSensorList(Sensor.TYPE_ALL)) {
            String name=(sensor.getName()+" "+sensor.getStringType()).toLowerCase(Locale.ROOT);
            if(sensor!=selected&&SensorChoice.luxSensor(sensor.getType(),sensor.getName(),sensor.getStringType())&&(name.contains("back")||name.contains("rear"))&&!sensor.isWakeUpSensor()){rear=sensor;break;}
        }
        if(selected!=null&&selected==rear)throw new IOException("front and rear cannot be the same sensor");
        registerSensors(false);front=selected;back=rear;proximity=sensors.getDefaultSensor(Sensor.TYPE_PROXIMITY);
        sensorChoice=choice;sensorConfigText=text;learningSource="";pending=null;manual.cancel();settingsLearning.reset();
    }
    String sourceScope(String source) throws Exception {
        if(source.equals("dual_max_v1"))return SensorChoice.scope(source+":"+front.getName()+":"+front.getVendor()+":"+back.getName()+":"+back.getVendor());
        if(source.equals("front")&&front==sensors.getDefaultSensor(Sensor.TYPE_LIGHT))return "default";
        return SensorChoice.scope(source+":"+(source.equals("front")?front.getName()+":"+front.getVendor():source.equals("rear")?back.getName()+":"+back.getVendor():sensorChoice.sysfs+":"+(front==null?"auto":front.getName())));
    }
    void probeOutput(long now) {
        OutputProbeGuard.Ticket ticket=outputProbe.begin(now);
        if(ticket==null)return;
        new Thread(()->{
            OutputProbeGuard.Result found=OutputProbeGuard.Result.UNKNOWN;boolean learning=false,observationLearning=false,slider=false;
            try{String dump=displayDump();found=ProtectionPolicy.referenceOutputResult(dump);learning=ProtectionPolicy.referenceLearningOutput(dump);observationLearning=ProtectionPolicy.referenceObservationLearningOutput(dump);slider=InteractionGuard.temporary(dump)==1;}catch(Exception ignored){}
            final OutputProbeGuard.Result value=found;final boolean learn=learning,observeLearn=observationLearning,drag=slider;
            final long completedAt=SystemClock.elapsedRealtime();
            handler.post(()->{if(closing.get())return;if(outputProbe.complete(ticket,completedAt,value)){outputLearningSafe=learn;observationLearningSafe=observeLearn;outputSliderActive=drag;if(value==OutputProbeGuard.Result.UNSAFE)wake();else wakeAmbient();}});
        },"iab-direct-protection").start();
    }
    void latchOutputFault(String reason) throws Exception {
        if(observe)return;
        if(outputFault.isEmpty()) {
            outputFault=reason;
            // Survive supervisor recovery and reboot. Only an explicit stopped
            // service start clears this marker, never a timer or a fresh probe.
            try{replace(new File(state,"output-fault"),reason+"\n");}
            finally{clearOverride();}
        }else clearOverride();
    }
    void manualLearning(long now,float value) throws Exception {
        // Settings polling is allowed only by explicit per-adjustment intent or
        // the opt-in compatibility mode. Framework events supply their own intent.
        String slider=systemSetting(user,Settings.System.SCREEN_BRIGHTNESS);
        String adjustment=systemSetting(user,"screen_auto_brightness_adj");
        boolean changed=!manual.origin.equals("event")&&(manual.settingChanged(manualSlider,slider)||manual.settingChanged(manualAdjustment,adjustment));
        manualSlider=slider;manualAdjustment=adjustment;
        if(changed&&eligible&&protectionReason.isEmpty()) {
            long wall=System.currentTimeMillis();manual.change(now,wall,learningLux.atEvent(wall,now,wall));
            changedAt=now;request("HOLD "+now);clearOverride();
        }
        boolean ready=manual.sample(now,eligible&&protectionReason.isEmpty(),value);
        eventStatus=manual.status;
        if(!ready||!driverLearningReady(now))return;
        if(learningLux.stillStable(manual.wall,manual.lux,now,System.currentTimeMillis())) {
            JSONObject saved=request("EVENT "+manual.wall+" "+manual.lux+" "+manual.value);
            eventStatus=saved.has("accepted")?"rejected_unstable":"saved_"+manual.origin;
        }else eventStatus="manual_discarded_lux_changed";
        manual.cancel();lastEvent=System.currentTimeMillis();
    }
    void settingsLearning(long now,float current) throws Exception {
        boolean allowed=result.optBoolean("settings_learning")&&result.optBoolean("learning_enabled")&&
            eligible&&protectionReason.isEmpty()&&pending==null&&result.optBoolean("enabled");
        if(!allowed){settingsLearning.reset();return;}
        String slider=systemSetting(user,Settings.System.SCREEN_BRIGHTNESS);
        String adjustment=systemSetting(user,"screen_auto_brightness_adj");
        if(settingsLearning.observe(now,allowed,slider,adjustment)) {
            long wall=System.currentTimeMillis();
            manualSlider=slider;manualAdjustment=adjustment;
            manual.begin(now,wall,learningLux.atEvent(wall,now,wall),"settings");
            changedAt=now;request("HOLD "+now);clearOverride();
            manualLearning(now,current);settingsLearning.reset();
        }
    }
    String displayDump() throws Exception {return serviceDump("display");}
    String serviceDump(String service) throws Exception {
        Process dump=new ProcessBuilder("/system/bin/dumpsys",service).redirectErrorStream(true).start();
        Thread timeout=new Thread(()->{SystemClock.sleep(5000);if(dump.isAlive())dump.destroyForcibly();},"iab-probe-timeout");timeout.setDaemon(true);timeout.start();
        StringBuilder body=new StringBuilder();
        try(Reader reader=new InputStreamReader(dump.getInputStream(),StandardCharsets.UTF_8)) {
            char[] b=new char[4096];int n;
            while((n=reader.read(b))>0){body.append(b,0,n);if(body.length()>2000000){dump.destroyForcibly();throw new IOException("display dump too large");}}
        }
        if(dump.waitFor()!=0)throw new IOException("display calibration probe failed");
        return body.toString();
    }
    boolean windowBlocked(Object info,long now) throws Exception {
        if(hasField(info,"isBrightnessOverrideByWindow"))return flag(info,"isBrightnessOverrideByWindow");
        if(!windowBusy&&now-windowRequested>=1000) {
            windowBusy=true;windowRequested=now;
            new Thread(()->{
                int found=-1;try{found=WindowPolicy.parse(serviceDump("power"));}catch(Exception ignored){}
                final int value=found;
                handler.post(()->{boolean changed=windowState!=value;windowState=value;windowAt=SystemClock.elapsedRealtime();windowBusy=false;if(changed)wake();});
            },"iab-window-status").start();
        }
        return windowState!=0||now-windowAt>10000;
    }
    float toBacklight(float nits) {
        if(nits<=profileNits[0])return profileBacklight[0];
        for(int i=1;i<profileNits.length;i++)if(nits<=profileNits[i])return profileBacklight[i-1]+(nits-profileNits[i-1])/(profileNits[i]-profileNits[i-1])*(profileBacklight[i]-profileBacklight[i-1]);
        return profileBacklight[profileBacklight.length-1];
    }
    void begin() throws Exception {
        Runtime.getRuntime().addShutdownHook(new Thread(this::shutdown,"iab-shutdown"));
        ContentObserver observer=new ContentObserver(handler) {
            @Override public void onChange(boolean selfChange) { onChange(selfChange,null); }
            @Override public void onChange(boolean selfChange,android.net.Uri uri) {
                if(closing.get())return;
                if(referenceControl){wake();return;}
                changedAt=SystemClock.elapsedRealtime();interaction.signal();wake();
                try{request("HOLD "+changedAt);clearOverride();}catch(Exception e){bridgeError=e.toString();}
            }
        };
        Method register=context.getContentResolver().getClass().getMethod("registerContentObserver",android.net.Uri.class,boolean.class,ContentObserver.class,int.class);
        for(String key:new String[]{Settings.System.SCREEN_BRIGHTNESS,Settings.System.SCREEN_BRIGHTNESS_MODE,"screen_auto_brightness_adj"})register.invoke(context.getContentResolver(),Settings.System.getUriFor(key),false,observer,-1);
        registerBrightnessListener(new DisplayManager.DisplayListener() {
            public void onDisplayAdded(int id) {if(id==0)wake();}
            public void onDisplayRemoved(int id) {if(id==0)wake();}
            public void onDisplayChanged(int id) {if(id==0)displayChanged();}
        });
        IntentFilter filter=new IntentFilter();
        for(String action:new String[]{Intent.ACTION_SCREEN_ON,Intent.ACTION_SCREEN_OFF,Intent.ACTION_BATTERY_CHANGED,Intent.ACTION_TIME_CHANGED,Intent.ACTION_TIMEZONE_CHANGED})filter.addAction(action);
        try{context.registerReceiver(new BroadcastReceiver(){public void onReceive(Context c,Intent i){
            if(Intent.ACTION_BATTERY_CHANGED.equals(i.getAction())) {
                int level=i.getIntExtra(BatteryManager.EXTRA_LEVEL,-1),scale=i.getIntExtra(BatteryManager.EXTRA_SCALE,-1);
                frameworkBattery=level>=0&&scale>0?Math.min(100,level*100/scale):-1;
                frameworkCharging=i.getIntExtra(BatteryManager.EXTRA_PLUGGED,0)!=0;
                int temp=i.getIntExtra(BatteryManager.EXTRA_TEMPERATURE,-1);batteryTemperature=temp>=0?temp/10f:-1;
            }
            if(Intent.ACTION_TIMEZONE_CHANGED.equals(i.getAction()))TimeZone.setDefault(null);
            if(Intent.ACTION_TIME_CHANGED.equals(i.getAction())){learningLux.reset();pending=null;manual.cancel();settingsLearning.reset();lastEvent=System.currentTimeMillis();}
            wake();
        }},filter);}
        catch(Exception e){System.err.println("broadcast listener unavailable: "+rootCause(e));}
        try{power.addThermalStatusListener(context.getMainExecutor(),status->{thermalStatus=status;wake();});}
        catch(Exception e){System.err.println("thermal listener unavailable; polling remains active");}
        new Thread(this::serve,"iab-control").start();
        Thread watchdog=new Thread(()->{
            while(!closing.get()) {
                SystemClock.sleep(1000);
                if(SystemClock.elapsedRealtime()-Math.max(heartbeat,nextTickAt)>5000) {
                    System.err.println("Bridge heartbeat expired; releasing display");
                    if(!observe&&new File(state,"direct-output.properties").isFile())try {
                        outputFault="heartbeat_timeout";replace(new File(state,"output-fault"),outputFault+"\n");
                    }catch(Exception ignored){}
                    // A stuck PANEL_READ may hold DirectBacklight's lock. Unblock
                    // the core pipe before attempting synchronized permission recovery.
                    core.destroyForcibly();
                    try { if(!observe)release(context,state,module); } catch(Exception ignored) {}
                    System.exit(1);
                }
            }
        },"iab-watchdog");watchdog.setDaemon(true);watchdog.start();
        handler.post(tickTask);
    }
    float adjusted(Object info) throws Exception {
        return number(info,hasField(info,"adjustedBrightness")?"adjustedBrightness":"brightness");
    }
    boolean externalBrightness(Object info,long now) throws Exception {
        if(referenceControl)return false;
        if(observe||info==null)return false;
        // Framework changes are external: direct writes never echo through this API.
        // Protection is handled separately and must never be mistaken for learning.
        if(power.getCurrentThermalStatus()!=PowerManager.THERMAL_STATUS_NONE
                ||(hasField(info,"brightnessMaxReason")&&integer(info,"brightnessMaxReason")!=0)
                ||(hasField(info,"isBrightnessOverrideByWindow")&&flag(info,"isBrightnessOverrideByWindow"))
                ||integer(info,"highBrightnessMode")==2
                ||(overridden&&number(info,"brightnessMaximum")<lastWrite))return false;
        if(direct!=null&&direct.locked()&&overridden&&!interaction.waiting()&&!interaction.ours(now,adjusted(info))) {
            // Framework auto/BCBC targets may move independently while we own the
            // node. Only confirmed automatic output can retain that ownership;
            // a drag, dim, override or unknown reason still takes the normal exit.
            String dump=displayDump();
            if(ProtectionPolicy.clearedTemporary(dump)) {
                interaction.wrote(now,adjusted(info),adjusted(info));return false;
            }
        }
        if(!interaction.observe(now,adjusted(info),overridden))return false;
        // Restore only if our last raw value is still present. A new SystemUI write wins.
        release(context,state,module);if(direct!=null)direct.released();
        overridden=false;lastWrite=-1;pending=null;manual.cancel();settingsLearning.reset();
        changedAt=now;result=request("HOLD "+now);return true;
    }
    void probeInteraction(long now) {
        if(!interaction.waiting()||interactionProbeBusy||now-interactionProbeAt<500)return;
        interactionProbeBusy=true;interactionProbeAt=now;
        final long epoch=interaction.epoch();
        new Thread(()->{
            int found=-1;try{found=InteractionGuard.temporary(displayDump());}catch(Exception ignored){}
            final int value=found;
            handler.post(()->{interactionProbeBusy=false;if(closing.get())return;interaction.probe(epoch,value);wake();});
        },"iab-interaction-check").start();
    }
    void clearOverride() throws Exception {
        if(!observe&&(overridden||(direct!=null&&(direct.owned()||direct.locked()))))release(context,state,module);
        overridden=false;lastWrite=-1;
        if(direct!=null&&(direct.owned()||direct.locked()))direct.released();
    }
    void displayChanged() {
        if(closing.get())return;
        if(referenceControl){wake();return;}
        try {
            Display main=display.getDisplay(0);
            if(main!=null&&main.getState()==Display.STATE_ON&&power.isInteractive()) {
                Object info=call(global,"getBrightnessInfo",new Class<?>[]{int.class},0);
                if(externalBrightness(info,SystemClock.elapsedRealtime())){wake();return;}
                // Our own acknowledgements must not drive a 50 ms feedback loop.
                if(overridden)return;
            }
        }catch(Exception e){bridgeError=rootCause(e);}
        wake();
    }
    void registerBrightnessListener(DisplayManager.DisplayListener listener) {
        try {
            long flags=DisplayManager.class.getField("EVENT_FLAG_DISPLAY_ADDED").getLong(null)
                |DisplayManager.class.getField("EVENT_FLAG_DISPLAY_REMOVED").getLong(null)
                |DisplayManager.class.getField("EVENT_FLAG_DISPLAY_CHANGED").getLong(null);
            try {
                long brightness=DisplayManager.class.getField("PRIVATE_EVENT_FLAG_DISPLAY_BRIGHTNESS").getLong(null);
                call(display,"registerDisplayListener",new Class<?>[]{DisplayManager.DisplayListener.class,Handler.class,long.class,long.class},listener,handler,flags,brightness);
            }catch(NoSuchFieldException|NoSuchMethodException absent) {
                long brightness=DisplayManager.class.getField("EVENT_FLAG_DISPLAY_BRIGHTNESS").getLong(null);
                call(display,"registerDisplayListener",new Class<?>[]{DisplayManager.DisplayListener.class,Handler.class,long.class},listener,handler,flags|brightness);
            }
            brightnessListener="brightness_events";
        }catch(Exception e){
            display.registerDisplayListener(listener,handler);
            System.err.println("Brightness listener unavailable; polling fallback: "+rootCause(e));
        }
    }
    void wake() {
        if(closing.get())return;
        // All calls are delivered on the main handler, including sensor events.
        cadence.reset();
        if(!wakeQueued) {
            wakeQueued=true;nextTickAt=SystemClock.elapsedRealtime()+50;
            handler.removeCallbacks(tickTask);handler.postDelayed(tickTask,50);
        }
    }
    void wakeAmbient() {
        if(!referenceControl){wake();return;}
        if(closing.get())return;
        long delay=Math.max(50,800-(SystemClock.elapsedRealtime()-referenceTickAt));
        if(nextTickAt<=SystemClock.elapsedRealtime()||nextTickAt>SystemClock.elapsedRealtime()+delay) {
            wakeQueued=true;nextTickAt=SystemClock.elapsedRealtime()+delay;
            handler.removeCallbacks(tickTask);handler.postDelayed(tickTask,delay);
        }
    }
    void registerLight(Sensor sensor,LightReading reading,int period) {
        if(reading==frontReading){learningLux.reset();pending=null;}
        if(sensor!=null)sensors.unregisterListener(this,sensor);
        boolean ok=sensor!=null&&sensors.registerListener(this,sensor,period,handler);
        reading.registration(ok,sensor!=null&&sensor.getReportingMode()==Sensor.REPORTING_MODE_ON_CHANGE);
    }
    void registerSensors(boolean wanted) {
        if(wanted==registered)return;
        sensorSession=SystemClock.elapsedRealtime();fallbackSample=null;cadence.reset();
        frontLux=-1;frontAt=0;near=false;
        if(wanted) {
            registerLight(front,frontReading,800000);registerLight(back,backReading,800000);
            if(proximity!=null)sensors.registerListener(this,proximity,SensorManager.SENSOR_DELAY_NORMAL,handler);
            lastSensorRetry=sensorSession;
            sensorStatus=frontReading.isRegistered()?front.getName():"front_unavailable";
        }else {
            sensors.unregisterListener(this);frontReading.registration(false,false);backReading.registration(false,false);
            learningLux.reset();pending=null;
            sensorStatus="suspended";fallbackStatus="idle";
        }
        registered=wanted;
    }
    void retrySensors(long now) {
        if(!registered||now-lastSensorRetry<30000)return;
        lastSensorRetry=now;
        if(!frontReading.isRegistered() || (front!=null&&front.getReportingMode()!=Sensor.REPORTING_MODE_ON_CHANGE&&frontReading.effective(now)<0)) {
            registerLight(front,frontReading,800000);frontLux=-1;frontAt=0;
            sensorStatus=frontReading.isRegistered()?front.getName():"front_unavailable";
        }
        if(!backReading.isRegistered() || (back!=null&&back.getReportingMode()!=Sensor.REPORTING_MODE_ON_CHANGE&&backReading.effective(now)<0))registerLight(back,backReading,800000);
    }
    void requestFallback(long now) {
        if(!registered||now-lastFallbackRequest<2000||!fallbackBusy.compareAndSet(false,true))return;
        lastFallbackRequest=now;final long session=sensorSession;
        fallbackStatus="reading";
        new Thread(()->{
            LuxFallback.Sample found=null;String status="unavailable";
            try {
                List<File> candidates=sensorChoice.sysfs.isEmpty()?LuxFallback.candidates(new File("/sys/class/sensors"),new File("/sys/bus/iio/devices")):Collections.singletonList(new File(sensorChoice.sysfs));
                if(candidates.size()==1) {
                    File path=candidates.get(0);float lux=LuxFallback.parseLux(read(path,128));
                    if(LuxFallback.valid(lux))found=new LuxFallback.Sample(lux,SystemClock.elapsedRealtime(),"sysfs:"+path.getPath());
                } else if(candidates.size()>1)status="ambiguous_sysfs";
                if(found==null&&sensorChoice.sysfs.isEmpty()) {
                    Process dump=new ProcessBuilder("/system/bin/dumpsys","sensorservice").redirectErrorStream(true).start();
                    Thread timeout=new Thread(()->{SystemClock.sleep(2000);if(dump.isAlive())dump.destroyForcibly();},"iab-fallback-timeout");timeout.setDaemon(true);timeout.start();
                    try(Reader reader=new InputStreamReader(dump.getInputStream(),StandardCharsets.UTF_8)) {
                        StringBuilder body=new StringBuilder();char[] buf=new char[4096];int count;
                        while((count=reader.read(buf))>0){body.append(buf,0,count);if(body.length()>2000000)throw new IOException("sensor dump too large");}
                        if(dump.waitFor()==0)found=LuxFallback.fromDump(body.toString(),front==null?null:front.getName(),SystemClock.elapsedRealtime(),session);
                    }finally{if(dump.isAlive())dump.destroyForcibly();}
                }
                if(found!=null)status=found.source;
            }catch(Exception e){status="unavailable: "+rootCause(e);}
            final LuxFallback.Sample sample=found;final String detail=status;
            handler.post(()->{
                fallbackBusy.set(false);
                if(!closing.get()&&registered&&sensorSession==session) {
                    fallbackSample=sample;fallbackStatus=detail;
                    if(sample!=null)wake();
                }
            });
        },"iab-lux-fallback").start();
    }
    @Override public void onSensorChanged(SensorEvent e) {
        if(!registered||e.values.length==0)return;
        float v=e.values[0];if(!Float.isFinite(v)||v<0)return;
        boolean changed=false;
        if(e.sensor==front){changed=PollCadence.changed(v,frontLux);frontLux=v;frontAt=SystemClock.elapsedRealtime();frontReading.update(v,frontAt);}
        else if(e.sensor==back){changed=PollCadence.changed(v,backReading.raw());backReading.update(v,SystemClock.elapsedRealtime());}
        else if(e.sensor==proximity){boolean value=v<Math.min(5,proximity.getMaximumRange());changed=near!=value;near=value;}
        if(changed){if(e.sensor==proximity)wake();else wakeAmbient();}
    }
    @Override public void onAccuracyChanged(Sensor s,int accuracy) {}
    static int activeUser() throws Exception {return (Integer)Class.forName("android.app.ActivityManager").getMethod("getCurrentUser").invoke(null);}
    long serial(int id) throws Exception {
        UserHandle handle=(UserHandle)UserHandle.class.getMethod("of",int.class).invoke(null,id);
        return ((UserManager)context.getSystemService(Context.USER_SERVICE)).getSerialNumberForUser(handle);
    }
    static String systemSetting(int uid,String key) throws Exception {
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
            Bundle values=(Bundle)call(provider,"call",new Class<?>[]{AttributionSource.class,String.class,String.class,String.class,Bundle.class},source,"settings","GET_system",key,extras);
            return values==null?null:values.getString("value");
        }finally{call(am,"removeContentProviderExternalAsUser",new Class<?>[]{String.class,IBinder.class,int.class},"settings",token,0);}
    }
    static int mode(int uid) throws Exception {
        String value=systemSetting(uid,Settings.System.SCREEN_BRIGHTNESS_MODE);
        return value==null?-1:Integer.parseInt(value);
    }
    void thermal(long now) {
        if(now-lastThermal<2000)return;lastThermal=now;
        try{battery=Integer.parseInt(read(new File("/sys/class/power_supply/battery/capacity"),32).trim());}catch(Exception e){battery=frameworkBattery;}
        try{charging=read(new File("/sys/class/power_supply/battery/status"),32).trim().equals("Charging");}catch(Exception e){charging=frameworkCharging;}
        if(thermalFiles.isEmpty()) {
            File[] zones=new File("/sys/class/thermal").listFiles();
            if(zones!=null)for(File z:zones)if(z.getName().startsWith("thermal_zone"))try{
                String type=read(new File(z,"type"),256).trim().toLowerCase(Locale.ROOT);
                // Include SoC alongside skin/battery sources, as in v6.8's boost gate.
                if(ThermalPolicy.boostSource(type))thermalFiles.add(new File(z,"temp"));
            }catch(Exception ignored){}
        }
        temperature=batteryTemperature>=0&&batteryTemperature<=150?batteryTemperature:-1;
        for(File f:thermalFiles)try{float t=Float.parseFloat(read(f,32).trim());if(t>1000)t/=1000;if(Float.isFinite(t)&&t>=0&&t<=150)temperature=Math.max(temperature,t);}catch(Exception ignored){}
    }
    void events(long now,boolean canLearn) {
        try {
            if(!eventCapability.probe(now)){if(!eventStatus.startsWith("saved"))eventStatus="manual_learning_available";return;}
            List<?> list=(List<?>)call(display,"getBrightnessEvents",new Class<?>[]{});
            long since=lastEvent,max=lastEvent;Object newest=null;float chosenLux=-1;
            long eventNow=SystemClock.elapsedRealtime(), wallNow=System.currentTimeMillis();
            for(Object e:list) {
                long ts=time(e,"timeStamp");max=Math.max(max,ts);
                if(ts<=since)continue;
                if(EventGate.controlIntent(ts,since,wallNow,integer(e,"userId"),user,string(e,"uniqueDisplayId"),displayId)) {
                    interaction.signal();changedAt=eventNow;request("HOLD "+eventNow);clearOverride();
                    pending=null;
                }
                float[] lux=(float[])e.getClass().getField("luxValues").get(e);
                long[] samples=(long[])e.getClass().getField("luxTimestamps").get(e);
                String rejection=EventGate.rejection(ts,since,wallNow,integer(e,"userId"),user,string(e,"uniqueDisplayId"),displayId,lux,samples,number(e,"brightness"),number(e,"powerBrightnessFactor"),flag(e,"reduceBrightColors"),canLearn);
                float direct=learningLux.atEvent(ts,eventNow,wallNow);
                if(rejection.isEmpty()&&direct<0)rejection="direct_lux_history";
                if(rejection.isEmpty()) {
                    if(newest==null||ts>time(newest,"timeStamp")){newest=e;chosenLux=direct;}
                }else {dropped++;lastEventRejection=rejection;}
            }
            eventCapability.success();lastEvent=max;
            if(newest!=null) {
                manual.cancel();settingsLearning.reset();
                float[] lux=(float[])newest.getClass().getField("luxValues").get(newest);
                lastFrameworkLux=lux[lux.length-1];lastLearningLux=chosenLux;
                if(!calibrated) {
                    // The event proves intent; stable normalized output supplies the
                    // value without pretending the generic nits curve is calibration.
                    if(wallNow-time(newest,"timeStamp")>3000){eventStatus="discarded_delayed_uncalibrated_event";return;}
                    manual.begin(now,time(newest,"timeStamp"),chosenLux,"event");
                    changedAt=now;request("HOLD "+now);clearOverride();eventStatus=manual.status;return;
                }
                pending=new JSONObject().put("time",time(newest,"timeStamp")).put("lux",chosenLux).put("value",toBacklight(number(newest,"brightness")));
                pendingAt=now;eventStatus="pending";request("HOLD "+now);clearOverride();
            }
            if(pending!=null&&now-pendingAt>10000){pending=null;eventStatus="discarded_driver_confirmation_timeout";}
            if(pending!=null&&!interaction.waiting()&&now-pendingAt>=1500&&driverLearningReady(eventNow)) {
                if(canLearn&&learningLux.stillStable(pending.getLong("time"),(float)pending.getDouble("lux"),eventNow,wallNow)) {
                    JSONObject learned=request("EVENT "+pending.getLong("time")+" "+pending.getDouble("lux")+" "+pending.getDouble("value"));
                    eventStatus=learned.has("accepted")?"rejected_unstable":"saved";
                }else eventStatus="discarded_state_or_lux_changed";
                pending=null;
            }
        }catch(Exception e){
            eventStatus="error: "+rootCause(e);
            eventCapability.failure(now,rootCause(e));pending=null;settingsLearning.reset();
        }
    }
    static String rootCause(Throwable e){while(e.getCause()!=null)e=e.getCause();return e.toString();}
    boolean driverLearningReady(long now) {
        // The core also rejects EVENT while the latest FRAME is blocked. Retain
        // intent until mapping/protection verification has cleared that frame.
        return !frameBlocked&&(observe||direct==null||direct.learning.learningReady(now));
    }
    void tick() {
        if(closing.get())return;
        long now=SystemClock.elapsedRealtime();heartbeat=now;nextTickAt=now;wakeQueued=false;referenceTickAt=now;
        try {
            int currentUser=activeUser();long currentSerial=serial(currentUser);
            if(currentSerial<0)throw new IOException("user serial unavailable");
            if(currentUser!=user||currentSerial!=userSerial) {
                clearOverride();user=currentUser;userSerial=currentSerial;pending=null;lastEvent=System.currentTimeMillis();
                learningLux.reset();manual.cancel();settingsLearning.reset();interaction.reset();
                sliderSampling.reset();
                if(direct!=null)direct.learning.reset();
                result=request("USER "+userSerial);
                File legacy=new File(state,"legacy-offset.dat");
                if(user==0&&(learningScope.isEmpty()||learningScope.equals("default"))&&result.optString("storage").equals("new")&&legacy.isFile())result=request("MIGRATE "+JSONObject.quote(legacy.getPath()));
            }
            boolean automatic=mode(user)==1;
            // HyperOS can report STATE_ON while an AOD pulse is still dozing.
            Display main=display.getDisplay(0);
            if(main!=null&&!displayId.equals((String)call(main,"getUniqueId",new Class<?>[]{}))) {
                System.err.println("Primary panel changed; restarting calibration");shutdown();System.exit(1);return;
            }
            boolean on=main!=null&&main.getState()==Display.STATE_ON&&power.isInteractive();
            boolean session=automatic&&on;
            if(session!=probeSession) {
                outputProbe.invalidate();outputLearningSafe=observationLearningSafe=outputSliderActive=false;probeSession=session;
                referenceAwakeAt=session?now:-1;sliderSampling.reset();
                if(direct!=null)direct.learning.reset();
            }
            registerSensors(automatic&&on);
            retrySensors(now);
            if(now-lastConfigCheck>=2000) {
                lastConfigCheck=now;
                File sensorFile=new File(state,"sensors.conf");String selection=sensorFile.isFile()?read(sensorFile,4096):"";
                if(!selection.equals(sensorConfigText)){clearOverride();loadSensorChoice();}
                String fresh=read(new File(state,"config.conf"),32768);
                if(!fresh.equals(configText)) {
                    configText=fresh;
                    try{request("RELOAD");}catch(Exception e){System.err.println("config: "+rootCause(e));}
                }
            }
            Object info=call(global,"getBrightnessInfo",new Class<?>[]{int.class},0);
            if(info==null)throw new IOException("brightness limits unavailable");
            float minimum=number(info,"brightnessMinimum"),maximum=number(info,"brightnessMaximum"),current=number(info,hasField(info,"adjustedBrightness")?"adjustedBrightness":"brightness");
            boolean blocked=(automatic&&on&&windowBlocked(info,now))||integer(info,"highBrightnessMode")==2;
            if(automatic&&on)externalBrightness(info,now);
            else interaction.reset();
            thermal(now);
            try{thermalStatus=power.getCurrentThermalStatus();}catch(Exception e){thermalStatus=-1;}
            brightnessMaxReason=hasField(info,"brightnessMaxReason")?integer(info,"brightnessMaxReason"):-1;
            protectionReason=protection.update(now,thermalStatus,ProtectionPolicy.capReason(android.os.Build.VERSION.SDK_INT,hasField(info,"brightnessMaxReason"),brightnessMaxReason));
            if(automatic&&on&&(direct!=null||observe)&&outputFault.isEmpty())probeOutput(now);
            OutputProbeGuard.State probeState=outputProbe.state(now,!observe&&session&&direct!=null&&direct.locked());
            if(probeState==OutputProbeGuard.State.FAULT)latchOutputFault("output_probe_timeout");
            boolean directReady=!observe&&direct!=null&&session&&outputFault.isEmpty()&&protectionReason.isEmpty()
                &&!blocked&&probeState!=OutputProbeGuard.State.UNSAFE&&direct.ready(now,current);
            if(direct!=null&&direct.faulted())latchOutputFault(direct.status);
            if(direct!=null)outputStatus=direct.status;
            // Direct writes bypass framework application of limits; require a cap reason
            // plus a recent unmodified automatic output and verified driver mapping.
            if(brightnessMaxReason<0)outputStatus="direct_cap_reason_unavailable";
            boolean directBlocked=!directReady||brightnessMaxReason<0||probeState!=OutputProbeGuard.State.SAFE||!outputFault.isEmpty();
            boolean learningBlocked=blocked||!protectionReason.isEmpty();
            boolean releaseRequired=learningBlocked||probeState==OutputProbeGuard.State.UNSAFE||!outputFault.isEmpty();
            blocked=learningBlocked||(!observe&&directBlocked);
            frameBlocked=blocked;
            if(!protectionReason.isEmpty()){eventStatus="paused_system_protection";clearOverride();pending=null;manual.cancel();settingsLearning.reset();lastEvent=System.currentTimeMillis();learningLux.reset();}
            // On-change light sensors may be silent while stable; registration stays active.
            float primary=frontReading.effective(now), backLux=backReading.effective(now);
            if(registered&&(!sensorChoice.sysfs.isEmpty()||(!LuxFallback.valid(primary)&&!LuxFallback.valid(backLux))))requestFallback(now);
            LuxFallback.Sample selected=LuxFallback.select(sensorChoice.sysfs.isEmpty()?primary:-1,sensorChoice.sysfs.isEmpty()?backLux:-1,fallbackSample,now,sensorSession);
            float lux=registered?selected.lux:-1;luxSource=registered?selected.source:"suspended";
            if(lux>=0) {
                String scope=sourceScope(luxSource);
                if(!scope.equals(learningScope)){if(!referenceControl)clearOverride();result=request("SOURCE "+scope);learningScope=scope;sliderSampling.reset();}
            }
            if(!luxSource.equals(learningSource)){learningLux.reset();pending=null;manual.cancel();settingsLearning.reset();lastEvent=System.currentTimeMillis();learningSource=luxSource;}
            learningLux.observe(lux,now,System.currentTimeMillis());
            eligible=automatic&&on&&!learningBlocked&&!near&&lux>=0;
            if(interaction.waiting()){request("HOLD "+now);probeInteraction(now);}
            result=request(String.format(Locale.ROOT,"FRAME3 %d %d %d %.8f %.8f %d %.4f %d %d %.9f %.9f %.9f %d %d %.8f",now,automatic?1:0,on?1:0,lux,backLux,near?1:0,temperature,battery,charging?1:0,(direct!=null&&!observe?direct.frameBrightness(current):current),minimum,maximum,blocked?1:0,Calendar.getInstance().get(Calendar.HOUR_OF_DAY),luxSource.equals("dual_max_v1")?primary:-1));
            if(result.optString("reason").startsWith("proximity")||result.optBoolean("ambient_guard")||!result.optBoolean("enabled")){eligible=false;learningLux.reset();}
            if(!automatic||!on||!result.optBoolean("enabled")){pending=null;manual.cancel();settingsLearning.reset();lastEvent=System.currentTimeMillis();}
            if(referenceControl) {
                // v6.8 samples adjacent settings every eight seconds. Require a
                // settled setting and exclude wake, DIM and output transitions.
                String slider=systemSetting(user,Settings.System.SCREEN_BRIGHTNESS);
                try {
                    int value=Integer.parseInt(slider);
                    referenceLearningAvailable=value>=0&&value<=255;
                    if(!referenceLearningAvailable)eventStatus="reference_slider_range_unsupported";
                    else {
                        boolean sampleSession=eligible&&result.optBoolean("enabled")&&result.optBoolean("learning_enabled")
                            &&referenceAwakeAt>=0&&now-referenceAwakeAt>=8000&&outputFault.isEmpty();
                        // Driver settling can precede the first temporary probe.
                        // Neither is a reason to consume a user's changed setting.
                        boolean learningProbeSafe=observe?observationLearningSafe:outputLearningSafe;
                        boolean learningProbeFresh=outputProbe.lastCompletedAt()>=0&&now-outputProbe.lastCompletedAt()<=OutputProbeGuard.FRESH_MS;
                        boolean discard=!sampleSession||(!observe&&(probeState==OutputProbeGuard.State.UNSAFE||probeState==OutputProbeGuard.State.FAULT))
                            ||(learningProbeFresh&&(probeState==OutputProbeGuard.State.SAFE||probeState==OutputProbeGuard.State.UNSAFE)&&!learningProbeSafe&&!outputSliderActive);
                        boolean wait=!discard&&(!learningProbeFresh||!learningProbeSafe||!driverLearningReady(now)||outputSliderActive);
                        ReferenceSliderSampling.Action action=wait?ReferenceSliderSampling.Action.NONE:sliderSampling.observe(now,value,!discard);
                        if(action==ReferenceSliderSampling.Action.BASELINE)result=request("SLIDER_BASELINE "+value);
                        else if(action==ReferenceSliderSampling.Action.SAMPLE) {
                            result=request("SLIDER "+value+" "+System.currentTimeMillis());eventStatus=result.optString("storage");
                        }
                    }
                }catch(NumberFormatException e){referenceLearningAvailable=false;eventStatus="reference_slider_unavailable";}
                pending=null;manual.cancel();settingsLearning.reset();interaction.reset();
            } else {
                // A real event supersedes heuristic capture; an explicitly armed
                // window retains its existing behavior on every ROM.
                if(!(manual.until>0&&manual.origin.equals("manual"))&&now-lastFrame>=250){events(now,eligible&&result.optBoolean("learning_enabled"));lastFrame=now;result=request("STATUS");}
                if(!result.optBoolean("learning_enabled")){pending=null;manual.cancel();settingsLearning.reset();}
                else if(!interaction.waiting()) {
                    if(manual.until>0){manualLearning(now,current);result=request("STATUS");settingsLearning.reset();}
                    else settingsLearning(now,current);
                }
            }
            if(result.optBoolean("apply")&&!interaction.waiting()&&now-changedAt>=2000&&pending==null&&manual.at<0&&!blocked&&!observe) {
                float target=(float)result.getDouble("target");
                Object latest=call(global,"getBrightnessInfo",new Class<?>[]{int.class},0);
                long writeNow=SystemClock.elapsedRealtime();
                Display active=display.getDisplay(0);
                boolean latestOn=active!=null&&active.getState()==Display.STATE_ON&&power.isInteractive();
                boolean latestAuto=mode(user)==1;
                boolean latestUnprotected=power.getCurrentThermalStatus()==0&&hasField(latest,"brightnessMaxReason")&&integer(latest,"brightnessMaxReason")==0
                    &&!windowBlocked(latest,writeNow)&&integer(latest,"highBrightnessMode")!=2;
                boolean systemSafe=latestOn&&latestAuto&&latestUnprotected;
                boolean safe=systemSafe&&outputProbe.state(writeNow,direct.locked())==OutputProbeGuard.State.SAFE&&outputFault.isEmpty();
                if(safe&&!externalBrightness(latest,writeNow)&&!interaction.waiting()&&direct.ready(writeNow,adjusted(latest))) {
                    boolean accepted=direct.write(target,number(latest,"brightnessMaximum"));
                    // Monitor system output independently of our raw-node target.
                    if(accepted){interaction.wrote(writeNow,adjusted(latest),adjusted(latest));lastWrite=direct.lastWritten()/(float)direct.maximum;}
                    else {result=request("OUTPUT_PAUSE");result.put("reason",direct.status);}
                    overridden=direct.owned();outputStatus=direct.status;
                }else {
                    result=request("OUTPUT_PAUSE");
                    outputStatus=direct.status;result.put("reason",safe?outputStatus:"direct_system_output_hold");
                    if(referenceControl&&!latestOn&&latestAuto&&latestUnprotected&&!releaseRequired)direct.suspend();
                    else if(releaseRequired||!systemSafe)clearOverride();
                }
                if(direct.faulted())latchOutputFault(direct.status);
            }else if(referenceControl&&automatic&&!on&&result.optBoolean("enabled")&&protectionReason.isEmpty()&&direct!=null&&direct.locked()) {
                direct.suspend();outputStatus=direct.status;
            }else if(!referenceControl||!automatic||!result.optBoolean("enabled")||releaseRequired)clearOverride();
            // A pending probe, fresh wake sensor, or non-atomic read holds output
            // without unlocking the node or resetting the controller.
            bridgeError="";
            boolean steady=result.optBoolean("apply")&&result.optBoolean("settled")&&pending==null&&manual.until==0&&now-changedAt>5000&&(luxSource.equals("front")||luxSource.equals("dual_max_v1"))&&(result.optBoolean("ambient_guard")||!PollCadence.changed(lux,(float)result.optDouble("lux",-1)));
            pollMs=registered?(near?8000:cadence.next(now,lux,steady,battery>=0&&battery<15&&!charging)):5000;
            if(result.optString("reason").equals("proximity_pending")||result.optString("reason").equals("proximity_recovery"))pollMs=100;
            if(eligible&&result.optBoolean("settings_learning"))pollMs=Math.min(pollMs,1000);
            // Reference input/output cadence is 800ms. Monitor frequently enough
            // to confirm driver transitions without a 100ms feedback chase loop.
            if(referenceControl&&automatic&&on)pollMs=result.optBoolean("apply")&&result.optBoolean("animating")?50:800;
            if(interaction.waiting())pollMs=100;
            else if(!referenceControl&&automatic&&on&&brightnessListener.equals("polling_fallback"))pollMs=Math.min(pollMs,250);
            if(interaction.waiting())result.put("reason","external_brightness_hold").put("apply",false);
            if(!observe&&automatic&&on&&directBlocked)result.put("reason",direct==null||!directReady||brightnessMaxReason<0?outputStatus:"direct_system_output_hold").put("apply",false);
            if(!protectionReason.isEmpty())result.put("reason",protectionReason).put("apply",false);
            if(!outputFault.isEmpty())result.put("reason","output_fault_latched").put("apply",false);
            result.put("boot_id",bootId).put("bridge_pid",android.os.Process.myPid())
                .put("bridge_start",Health.processStart(android.os.Process.myPid())).put("core_start",Health.processStart(result.optLong("core_pid",-1)))
                .put("output_backend","direct_sysfs").put("output_status",outputStatus)
                .put("output_fault",outputFault).put("output_probe_state",probeState.toString().toLowerCase(Locale.ROOT))
                .put("output_probe_started_ms",outputProbe.lastStartedAt()).put("output_probe_completed_ms",outputProbe.lastCompletedAt())
                .put("output_probe_result",outputProbe.lastResult().toString().toLowerCase(Locale.ROOT)).put("output_probe_generation",outputProbe.generation())
                .put("backlight_write_locked",direct!=null&&direct.locked())
                .put("driver_learning_status",direct==null||observe?"not_applicable":direct.learning.status)
                .put("backlight_node",direct==null?"":direct.path()).put("backlight_requested",direct==null?-1:direct.requested())
                .put("backlight_actual",direct==null?-1:direct.actual()).put("backlight_maximum",direct==null?-1:direct.maximum)
                .put("protection_signals",hasField(info,"brightnessMaxReason")?"thermal_and_cap_reason":"thermal_and_system_cap_only")
                .put("thermal_status",thermalStatus).put("brightness_max_reason",brightnessMaxReason).put("protection",protectionReason.isEmpty()?"clear":protectionReason)
                .put("manual_learning_available",!referenceControl).put("manual_learning_armed",manual.until>now).put("learning_scope",learningScope).put("sensor_selection",sensorSelectionStatus)
                .put("observation_only",observe).put("bridge","active").put("event_status",eventStatus).put("events_dropped",dropped)
                .put("brightness_listener",brightnessListener).put("interaction_guard",interaction.status)
                .put("proximity_near",near).put("input_lux",lux).put("fusion_policy",luxSource.equals("dual_max_v1")?"brighter_valid_side":"single_source")
                .put("last_event_rejection",lastEventRejection).put("lux_source",luxSource).put("fallback_status",fallbackStatus)
                .put("calibration",calibrationStatus).put("learning_available",referenceLearningAvailable).put("event_capability",eventCapability.status).put("learning_value_source","reference_slider_gamma").put("control_policy","v68").put("learning_model","bounded_curve_offset").put("window_override_detection",hasField(info,"isBrightnessOverrideByWindow")?"framework":("power_dump:"+windowState))
                .put("learning_lux",lastLearningLux).put("framework_event_lux",lastFrameworkLux)
                .put("poll_ms",pollMs).put("front_registered",frontReading.isRegistered()).put("front_sample_age_ms",frontReading.age(now))
                .put("sensor",sensorStatus).put("back_sensor",back==null?"unavailable":back.getName()).put("proximity_sensor",proximity==null?"unavailable":proximity.getName())
                .put("front_lux",frontLux).put("back_lux",backReading.raw()).put("back_lux_effective",backLux)
                .put("back_sample_age_ms",backReading.age(now)).put("back_registered",backReading.isRegistered())
                .put("back_reporting_mode",back==null?-1:back.getReportingMode())
                .put("temperature",temperature).put("battery",battery).put("mode",automatic?"auto":"manual_or_unknown").put("screen_on",on)
                .put("slider",systemSetting(user,Settings.System.SCREEN_BRIGHTNESS)).put("version",version).put("source_commit",sourceCommit).put("current",current).put("setting",number(info,"brightness")).put("minimum",minimum).put("maximum",maximum).put("display",0).put("uptime_ms",now-started).put("heartbeat_ms",now).put("cpu_percent",100.0*(android.os.Process.getElapsedCpuTime()-cpuStarted)/Math.max(1,now-started));
            String reason=result.optString("reason");
            float target=(float)result.optDouble("target",-1);
            boolean significant=lastLoggedTarget<0||Math.abs(target-lastLoggedTarget)>Math.max(.0005f,Math.abs(lastLoggedTarget)*.08f);
            if(!reason.equals(previousReason)||(significant&&now-lastDecisionLog>=1000)) {
                System.out.println(String.format(Locale.ROOT,"time=%tFT%<tT%<tz state=%s source=%s front=%.3f rear=%.3f smooth=%.3f target=%.7f current=%.7f cap=%.7f temp=%.1f battery=%d poll=%d storage=%s raw=%d actual=%d probe=%s probe_start=%d probe_done=%d fault=%s",new Date(),reason,luxSource,primary,backLux,result.optDouble("lux",-1),target,current,maximum,temperature,battery,pollMs,result.optString("storage"),result.optLong("backlight_requested",-1),result.optLong("backlight_actual",-1),probeState,outputProbe.lastStartedAt(),outputProbe.lastCompletedAt(),outputFault));
                previousReason=reason;lastLoggedTarget=target;lastDecisionLog=now;
            }
            if(now-lastStatus>=1000){replace(new File(state,"status.json"),result.toString());lastStatus=now;}
        }catch(Exception e) {
            eligible=false;manual.cancel();pending=null;settingsLearning.reset();
            String error=rootCause(e);if(!error.equals(bridgeError))System.err.println("bridge: "+error);bridgeError=error;
            try{if(!observe&&direct!=null&&(overridden||direct.owned()||direct.locked()||direct.faulted()))latchOutputFault("bridge_output_error: "+error);else clearOverride();}catch(Exception ignored){}
            try{result=new JSONObject().put("ok",false).put("bridge","degraded").put("error",bridgeError).put("heartbeat_ms",now);replace(new File(state,"status.json"),result.toString());}catch(Exception ignored){}
            if(!core.isAlive()) {shutdown();System.exit(1);}
        }finally{heartbeat=SystemClock.elapsedRealtime();if(!closing.get()){nextTickAt=heartbeat+pollMs;handler.removeCallbacks(tickTask);handler.postDelayed(tickTask,pollMs);}}
    }
    JSONObject control(String line) throws Exception {
        String[] parts=line.split(" ",2);String cmd=parts[0];
        if(cmd.equals("status"))return new JSONObject(result.toString());
        if(cmd.equals("log-clear")) {
            try(FileOutputStream out=new FileOutputStream(new File(state,"service.log"),false)){out.getFD().sync();}
            return new JSONObject().put("ok",true);
        }
        if(cmd.equals("learn-arm")) {
            if(referenceControl)throw new IOException("参考模式直接根据滑块变化采集曲线偏好，无需开启学习窗口");
            tick();
            if(!eligible||!protectionReason.isEmpty()||result.optBoolean("ambient_guard")||!result.optBoolean("settled"))throw new IOException("screen, automatic mode and stable unprotected lux required");
            manualSlider=systemSetting(user,Settings.System.SCREEN_BRIGHTNESS);manualAdjustment=systemSetting(user,"screen_auto_brightness_adj");
            manual.arm(SystemClock.elapsedRealtime());pending=null;eventStatus="manual_armed_adjust_slider";wake();
            return new JSONObject().put("ok",true);
        }
        if(cmd.equals("config-get"))return request("CONFIG_GET");
        if(cmd.equals("learn-export"))return request("EXPORT");
        if(cmd.equals("learn-clear")){pending=null;manual.cancel();return request("CLEAR");}
        if(cmd.equals("stop")){handler.postDelayed(()->{shutdown();System.exit(0);},200);return new JSONObject().put("ok",true);}
        if(cmd.equals("config-set")||cmd.equals("learn-import")) {
            if(parts.length!=2)throw new IOException("payload required");
            byte[] decoded=android.util.Base64.decode(parts[1],android.util.Base64.DEFAULT);
            if(decoded.length>32768)throw new IOException("payload too large");
            File temp=File.createTempFile(".request-",".tmp",state);
            try {try(FileOutputStream out=new FileOutputStream(temp)){out.write(decoded);out.getFD().sync();}
                JSONObject reply=request((cmd.equals("config-set")?"CONFIG ":"IMPORT ")+JSONObject.quote(temp.getPath()));
                pending=null;manual.cancel();wake();return reply;
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
        try{if(!observe)release(context,state,module);}catch(Exception e){System.err.println("release: "+e);}
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
        if(args.length==3&&args[0].equals("health")){System.out.println(Health.report(new File(args[1]),new File(args[2])));return;}
        if(args.length==3&&args[0].equals("upgrade-ready")){System.out.println(Health.upgradeReady(new File(args[1]),new File(args[2])));return;}
        if(args.length==1&&args[0].equals("backlight")) {
            org.json.JSONArray rows=new org.json.JSONArray();
            for(Backlight.Row row:Backlight.read(new File("/sys/class/backlight")))rows.put(new JSONObject().put("path",row.path).put("requested",row.requested).put("actual",row.actual).put("maximum",row.maximum));
            System.out.println(new JSONObject().put("ok",true).put("nodes",rows));return;
        }
        if(args.length==1&&args[0].equals("release")) {
            DirectBacklight.unlock(BACKLIGHT_ROOT,new File("/data/adb/ios_auto_brightness/direct-output.properties"),read(new File("/proc/sys/kernel/random/boot_id"),128).trim());
        }
        Looper.prepareMainLooper();Context context=systemContext();
        if(args.length>=2&&(args[0].equals("sensors-get")||args[0].equals("sensors-save"))) {
            SensorManager manager=(SensorManager)context.getSystemService(Context.SENSOR_SERVICE);
            File file=new File(args[1],"sensors.conf");
            if(args[0].equals("sensors-save")) {
                if(args.length!=3||args[2].length()>8192)throw new IOException("selection payload required");
                SensorChoice choice=new SensorChoice(new String(android.util.Base64.decode(args[2],android.util.Base64.DEFAULT),StandardCharsets.UTF_8));
                validateChoice(manager,choice);
                File temp=File.createTempFile(".sensors-",".tmp",file.getParentFile());
                try {
                    try(FileOutputStream out=new FileOutputStream(temp)){out.write(choice.encode().getBytes(StandardCharsets.UTF_8));out.getFD().sync();}
                    Files.move(temp.toPath(),file.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
                }finally{temp.delete();}
                System.out.println(new JSONObject().put("ok",true));
            }else {
                org.json.JSONArray list=new org.json.JSONArray();
                for(Sensor sensor:manager.getSensorList(Sensor.TYPE_ALL)) {
                    if(SensorChoice.luxSensor(sensor.getType(),sensor.getName(),sensor.getStringType()))list.put(sensor.getName());
                }
                System.out.println(new JSONObject().put("ok",true).put("data",file.isFile()?read(file,4096):new SensorChoice("").encode()).put("sensors",list));
            }return;
        }
        if(args.length==1&&args[0].equals("release")){release(context,new File("/data/adb/ios_auto_brightness"),new File("/data/adb/modules/ios_auto_brightness"));return;}
        if(args.length<2||args.length>3)throw new IllegalArgumentException("MODULE STATE [--observe]");
        Bridge bridge=new Bridge(context,new File(args[0]),new File(args[1]),args.length==3&&args[2].equals("--observe"));
        bridge.begin();Looper.loop();
    }
}
