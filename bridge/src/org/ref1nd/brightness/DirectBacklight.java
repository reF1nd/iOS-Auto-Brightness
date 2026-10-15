// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/** Direct sysfs writer with an optional recoverable write-permission lease. */
public final class DirectBacklight {
    public interface Readback {long read() throws IOException;}
    private final Readback readback;
    private final File node, journal;
    private final BacklightLease lease;
    private float baselineFramework=Float.NaN;
    private final String display, boot;
    public final long maximum;
    public final DriverOutputGuard learning=new DriverOutputGuard();
    private long last=-1, stableAt=-1, retryAt=0, baseline=-1;
    public String status="verifying_mapping";
    public DirectBacklight(File root,String name,File journal,String display,String boot) throws IOException {
        this(root,name,journal,display,boot,null);
    }
    public DirectBacklight(File root,String name,File journal,String display,String boot,Readback readback) throws IOException {
        this(root,name,journal,display,boot,readback,null);
    }
    public DirectBacklight(File root,String name,File journal,String display,String boot,Readback readback,BacklightLease.Access permissions) throws IOException {
        this.readback=readback;
        if(!name.matches("[A-Za-z0-9_-]+"))throw new IOException("invalid backlight node name");
        this.node=new File(root,name);this.journal=journal;this.display=display;this.boot=boot;
        lease=permissions==null?null:new BacklightLease(root,name,permissionJournal(journal),boot,permissions);
        maximum=Backlight.number(new File(node,"max_brightness"));
        if(maximum<2||maximum>1000000||!new File(node,"brightness").canWrite())throw new IOException("backlight range or write access unavailable");
    }
    public String path(){return node.getPath();}
    public boolean owned(){return last>=0;}
    public boolean locked(){return lease!=null&&lease.active();}
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
    private static boolean close(long a,long b){return a>=0&&b>=0&&Math.abs(a-b)<=1;}
    public boolean ready(long now,float framework) throws IOException {
        synchronized(DirectBacklight.class) {
        long raw=requested(),actual=actual();
        long liveMaximum=Backlight.number(new File(node,"max_brightness"));
        learning.sample(now,raw,actual,liveMaximum==maximum?maximum:-1,framework);
        if(locked()&&!lease.intact()){abandon(now,"backlight_permissions_changed");return false;}
        if(liveMaximum!=maximum||raw<0||raw>maximum||actual<0||actual>maximum) {
            abandon(now,"backlight_readback_unavailable");return false;
        }
        if(owned()&&raw!=last){abandon(now,"external_backlight_write");return false;}
        // A lower driver output can be an unreported vendor protection. Do not retry it.
        if(!close(actual,raw)){abandon(now,"driver_output_mismatch");return false;}
        if(owned()){status="direct_active";return true;}
        long expected=units(framework,maximum);
        // The verified Xiaomi compositor applies a small compensation before sysfs.
        // Keep it bounded; never fit a guessed global transform from a single sample.
        long tolerance=readback==null?1:Math.max(16,Math.round(expected*.02));
        if(Math.abs(raw-expected)>tolerance) {
            stableAt=-1;status="backlight_mapping_mismatch";return false;
        }
        if(raw!=baseline){baseline=raw;stableAt=now;}
        if(stableAt<0)stableAt=now;
        if(now<retryAt||now-stableAt<1000){status="verifying_mapping";return false;}
        baselineFramework=framework;status="direct_ready";return true;
        }
    }
    private void abandon(long now,String reason) throws IOException {
        if(lease!=null)lease.release();
        last=-1;stableAt=-1;baseline=-1;retryAt=now+2000;status=reason;Files.deleteIfExists(journal.toPath());
    }
    public void write(float target,float cap) throws IOException {
        synchronized(DirectBacklight.class) {
        long raw=units(Math.min(target,cap),maximum);
        if(raw<1)throw new IOException("refusing to blank an active display");
        if(Backlight.number(new File(node,"max_brightness"))!=maximum)throw new IOException("backlight range changed");
        // Recheck after IPC. If the system wrote in the meantime, leave its value alone.
        long before=requested();
        if((owned()?before!=last:before!=baseline)||!close(actual(),before))throw new IOException("backlight ownership changed before write");
        if(owned()&&raw==last)return;
        if(lease!=null)lease.acquire();
        Properties p=new Properties();p.setProperty("node",node.getName());p.setProperty("display",display);
        p.setProperty("boot",boot);p.setProperty("maximum",Long.toString(maximum));p.setProperty("last",Long.toString(raw));
        p.setProperty("readback",readback==null?"sysfs":"mi");p.setProperty("systemRaw",Long.toString(baseline));p.setProperty("framework",Float.toString(baselineFramework));
        File tmp=File.createTempFile(".direct-",".tmp",journal.getParentFile());
        try {
            try(FileOutputStream out=new FileOutputStream(tmp)){p.store(out,"IAB direct output recovery");out.getFD().sync();}
            Files.move(tmp.toPath(),journal.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
        }finally{tmp.delete();}
        try {writeNode(new File(node,"brightness"),raw);last=raw;status="direct_active";}
        catch(IOException e){last=-1;throw e;}
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
    public void released()throws IOException {if(lease!=null)lease.release();last=-1;stableAt=-1;baseline=-1;status="released";}
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
