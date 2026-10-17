// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/** Change-driven sysfs writer with a recoverable, non-exclusive permission lease. */
public final class DirectBacklight {
    public interface Readback {long read() throws IOException;}
    private final Readback readback;
    private final boolean verifiedScale;
    private final File node, journal;
    private final BacklightLease lease;
    private RandomAccessFile writer;
    private boolean reference=false, fault=false, holding=false;
    private float baselineFramework=Float.NaN;
    private final String display, boot;
    public final long maximum;
    public final DriverOutputGuard learning=new DriverOutputGuard();
    private long last=-1, stableAt=-1, baseline=-1, observed=-1, sampledAt=-1;
    private float sampledFramework=Float.NaN;
    public String status="verifying_mapping";
    public DirectBacklight(File root,String name,File journal,String display,String boot) throws IOException {
        this(root,name,journal,display,boot,null);
    }
    public DirectBacklight(File root,String name,File journal,String display,String boot,Readback readback) throws IOException {
        this(root,name,journal,display,boot,readback,null);
    }
    public DirectBacklight(File root,String name,File journal,String display,String boot,Readback readback,BacklightLease.Access permissions) throws IOException {
        this(root,name,journal,display,boot,readback,permissions,false);
    }
    /** verifiedScale is only supplied by an independently identified panel adapter. */
    public DirectBacklight(File root,String name,File journal,String display,String boot,Readback readback,BacklightLease.Access permissions,boolean verifiedScale) throws IOException {
        if(verifiedScale&&readback==null)throw new IOException("verified scale requires independent driver readback");
        this.readback=readback;this.verifiedScale=verifiedScale;
        if(!name.matches("[A-Za-z0-9_-]+"))throw new IOException("invalid backlight node name");
        this.node=new File(root,name);this.journal=journal;this.display=display;this.boot=boot;
        lease=permissions==null?null:new BacklightLease(root,name,permissionJournal(journal),boot,permissions);
        maximum=Backlight.number(new File(node,"max_brightness"));
        if(maximum<2||maximum>1000000||!new File(node,"brightness").canWrite())throw new IOException("backlight range or write access unavailable");
    }
    public void referenceMode(){reference=true;}
    /** Keep the lease through sleep, but never write an off/AOD panel. */
    public void suspend(){if(!fault)status="reference_screen_off";stableAt=-1;observed=-1;holding=false;learning.reset();}
    public String path(){return node.getPath();}
    public boolean owned(){return last>=0;}
    /** Last value this writer actually submitted; it is not a driver measurement. */
    public long lastWritten(){return last;}
    public boolean locked(){return lease!=null&&lease.active();}
    /** Only an explicit service restart/new writer clears a latched fault. */
    public boolean faulted(){return fault;}
    public boolean holding(){return holding&&!fault;}
    static File permissionJournal(File journal){return new File(journal.getParentFile(),"backlight-permissions.properties");}
    public static synchronized void unlock(File root,File journal,String boot)throws IOException {
        BacklightLease.restore(root,permissionJournal(journal),boot,BacklightLease.POSIX);
    }
    public long requested(){return Backlight.number(new File(node,"brightness"));}
    public long actual(){try{return readback==null?Backlight.number(new File(node,"actual_brightness")):readback.read();}catch(IOException e){return -1;}}
    public static long units(float value,long maximum) {
        if(!Float.isFinite(value)||value<0||value>1)throw new IllegalArgumentException("invalid normalized brightness");
        return Math.round((double)value*maximum);
    }
    /** Seed the controller from the verified live output even before its first write. */
    public float frameBrightness(float framework) {
        return !fault&&observed>=0?observed/(float)maximum:framework;
    }
    private static boolean close(long a,long b){return a>=0&&b>=0&&Math.abs(a-b)<=1;}
    private static final class Snapshot {
        final long before,actual,after,maximum;
        Snapshot(long before,long actual,long after,long maximum){this.before=before;this.actual=actual;this.after=after;this.maximum=maximum;}
    }
    private Snapshot snapshot() {
        long before=requested(),actual=actual(),after=requested();
        return new Snapshot(before,actual,after,Backlight.number(new File(node,"max_brightness")));
    }
    private boolean validate(Snapshot sample,long now,float framework)throws IOException {
        learning.sample(now,sample.before,sample.actual,sample.after,sample.maximum==maximum?maximum:-1,framework);
        if(locked()&&!lease.intact()){latch("backlight_permissions_changed");return false;}
        if(sample.maximum!=maximum||sample.before<0||sample.before>maximum||sample.after<0||sample.after>maximum||sample.actual<0||sample.actual>maximum) {
            latch("backlight_readback_unavailable");return false;
        }
        if(sample.before!=sample.after||!close(sample.actual,sample.after)) {
            holding=true;status="driver_output_transition";
            if(learning.protection.equals("driver_output_mismatch"))latch("driver_output_mismatch");
            return false;
        }
        observed=sample.after;holding=false;
        return true;
    }
    public boolean ready(long now,float framework) throws IOException {
        synchronized(DirectBacklight.class) {
        if(fault)return false;
        sampledAt=now;sampledFramework=framework;
        try {
        Snapshot sample=snapshot();
        if(!validate(sample,now,framework))return false;
        long raw=sample.after;
        if(owned()&&raw!=last&&!reference){latch("external_backlight_write");return false;}
        if(owned()){status="direct_active";return true;}
        long expected=units(framework,maximum);
        // A proven panel scale does not require the OEM's final compensated output
        // to equal its framework target. Unknown adapters still need a live match.
        if(!verifiedScale&&Math.abs(raw-expected)>1) {
            stableAt=-1;holding=true;status="backlight_mapping_mismatch";return false;
        }
        if(raw!=baseline){baseline=raw;stableAt=now;}
        if(stableAt<0)stableAt=now;
        if(now-stableAt<1000){holding=true;status="verifying_mapping";return false;}
        baselineFramework=framework;status="direct_ready";return true;
        }catch(IOException|RuntimeException e){if(!fault)latch("backlight_io_failure");throw e;}
        }
    }
    private void latch(String reason) throws IOException {
        if(fault)return;
        fault=true;holding=false;status=reason;last=-1;stableAt=-1;baseline=-1;observed=-1;
        // Never restore brightness after a conflict. Permission recovery remains
        // durable if releasing the lease fails, for supervisor/startup cleanup.
        try{releaseAccess();}finally{Files.deleteIfExists(journal.toPath());}
    }
    private void releaseAccess()throws IOException {
        try{if(writer!=null)writer.close();}
        finally{writer=null;if(lease!=null&&lease.active())lease.release();}
    }
    /** true means accepted (possibly unchanged); false means no write during a hold/fault. */
    public boolean write(float target,float cap) throws IOException {
        synchronized(DirectBacklight.class) {
        if(fault||holding)return false;
        long raw=units(Math.min(target,cap),maximum);
        // Acquire without a brightness step; following frames use the normal ramp.
        if((verifiedScale||reference)&&!owned()&&baseline>=0)raw=Math.min(baseline,units(cap,maximum));
        if(raw<1)throw new IOException("refusing to blank an active display");
        // v6.8 compares its cached output, not the live node: unchanged targets
        // must not chase writes from an already-open system descriptor.
        if(owned()&&raw==last)return true;
        try {
        long previousObserved=observed;
        Snapshot sample=snapshot();
        if(!validate(sample,sampledAt,sampledFramework))return false;
        if(sample.after!=previousObserved||(owned()&&!reference&&sample.after!=last)) {
            holding=true;status="backlight_output_transition";stableAt=-1;
            return false;
        }
        if(lease!=null) {
            // app_process can lack DAC override despite uid 0. Retain a handle
            // opened while writable rather than reopening after chmod 0444.
            // This restricts new opens, but is not exclusive ownership: existing
            // descriptors held by the display service remain writable too.
            if(writer==null)writer=new RandomAccessFile(new File(node,"brightness"),"rw");
            lease.acquire();
        }
        Properties p=new Properties();p.setProperty("node",node.getName());p.setProperty("display",display);
        p.setProperty("boot",boot);p.setProperty("maximum",Long.toString(maximum));p.setProperty("last",Long.toString(raw));
        p.setProperty("readback",readback==null?"sysfs":"mi");p.setProperty("systemRaw",Long.toString(baseline));p.setProperty("framework",Float.toString(baselineFramework));
        if(reference)p.setProperty("policy","reference_no_restore");
        File tmp=File.createTempFile(".direct-",".tmp",journal.getParentFile());
        try {
            try(FileOutputStream out=new FileOutputStream(tmp)){p.store(out,"IAB direct output recovery");out.getFD().sync();}
            Files.move(tmp.toPath(),journal.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
        }finally{tmp.delete();}
        if(writer!=null){writer.seek(0);writer.write((raw+"\n").getBytes(StandardCharsets.US_ASCII));}
        else writeNode(new File(node,"brightness"),raw);
        last=raw;status="direct_active";return true;
        }catch(IOException|RuntimeException e){if(!fault)latch("backlight_io_failure");throw e;}
        }
    }
    static void writeNode(File path,long value) throws IOException {
        try(FileOutputStream out=new FileOutputStream(path)){out.write((value+"\n").getBytes(StandardCharsets.US_ASCII));}
    }
    /** Restore only our unchanged last write; never replace a newer system/user write. */
    public static void restore(File root,File journal,String display,String boot,boolean on,boolean canRaise,float framework,float cap) throws IOException {
        restore(root,journal,display,boot,on,canRaise,framework,cap,null);
    }
    public static synchronized void restore(File root,File journal,String display,String boot,boolean on,boolean canRaise,float framework,float cap,Readback readback) throws IOException {
        unlock(root,journal,boot);
        if(!journal.isFile())return;
        Properties p=new Properties();try(InputStream in=new FileInputStream(journal)){p.load(in);}
        try {
            // Reference mode never substitutes a stale framework target on exit.
            // Older journals without this policy keep their original recovery.
            if("reference_no_restore".equals(p.getProperty("policy")))return;
            if(!display.equals(p.getProperty("display"))||!boot.equals(p.getProperty("boot"))||!on)return;
            String name=p.getProperty("node","");if(!name.matches("[A-Za-z0-9_-]+"))return;
            File node=new File(root,name);long max=Long.parseLong(p.getProperty("maximum")),last=Long.parseLong(p.getProperty("last"));
            long raw=Backlight.number(new File(node,"brightness"));
            long actual="mi".equals(p.getProperty("readback"))?(readback==null?-1:readback.read()):Backlight.number(new File(node,"actual_brightness"));
            if(max<2||max>1000000||last<1||last>max||Backlight.number(new File(node,"max_brightness"))!=max||raw!=last)return;
            // An unknown/lower driver output wins over recovery, including after a crash.
            if(!close(actual,raw))return;
            long target=units(Math.min(framework,cap),max);
            float saved=Float.parseFloat(p.getProperty("framework","NaN"));
            long original=Long.parseLong(p.getProperty("systemRaw","-1"));
            if(Float.isFinite(saved)&&Math.abs(framework-saved)<.000001f&&original>0&&original<=max)
                target=Math.min(original,units(cap,max));
            if(!canRaise)target=Math.min(target,raw);
            if(target>=1&&target!=raw)writeNode(new File(node,"brightness"),target);
        }catch(IllegalArgumentException e){throw new IOException("invalid backlight recovery data",e);}
        finally{Files.deleteIfExists(journal.toPath());}
    }
    public void released()throws IOException {
        releaseAccess();last=-1;stableAt=-1;baseline=-1;observed=-1;holding=false;
        if(!fault)status="released";
    }
    /** Framework -> HAL identity must be proven, never assume two normalized ranges match. */
    public static boolean identityMapping(String dump,String display) {
        try {
            int at=dump.indexOf("uniqueId=\""+display+"\"");if(at<0)return false;
            int end=dump.indexOf("DisplayDeviceInfo{",at);String s=dump.substring(at,end<0?dump.length():end);
            float[] b=PanelProfile.array(s,"mBrightness"),h=PanelProfile.array(s,"mBacklight");
            if(b.length<2||b.length!=h.length)return false;
            for(int i=0;i<b.length;i++)if(!Float.isFinite(b[i])||!Float.isFinite(h[i])||b[i]<0||b[i]>1||Math.abs(b[i]-h[i])>.0000001f||(i>0&&b[i]<=b[i-1]))return false;
            return true;
        }catch(Exception e){return false;}
    }
}
