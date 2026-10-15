// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Strict parsing and source selection, independent of Android services. */
public final class LuxFallback {
    public static final class Sample {
        public final float lux;
        public final long at;
        public final long verifiedAt;
        public final String source;
        public Sample(float lux, long at, String source) { this(lux,at,source,at); }
        private Sample(float lux,long at,String source,long verifiedAt){this.lux=lux;this.at=at;this.source=source;this.verifiedAt=verifiedAt;}
        public boolean usable(long now, long sessionStart) {
            return valid(lux) && at>=sessionStart && at<=now && verifiedAt<=now && now-verifiedAt<=15000;
        }
    }
    public static boolean valid(float lux) { return Float.isFinite(lux)&&lux>=0&&lux<=200000; }
    public static float parseLux(String text) {
        try {float value=Float.parseFloat(text.trim());return valid(value)?value:-1;}
        catch(RuntimeException e){return -1;}
    }
    public static Sample select(float front, float rear, Sample fallback, long now, long sessionStart) {
        if(valid(front))return new Sample(front,now,"front");
        if(valid(rear))return new Sample(rear,now,"rear");
        if(fallback!=null&&fallback.usable(now,sessionStart))return fallback;
        return new Sample(-1,now,"unavailable");
    }
    public static Sample fromDump(String text, String sensorName, long now, long sessionStart) {
        if(sensorName==null||sensorName.isEmpty()) {
            String chosen=null;
            for(String line:text.split("\\r?\\n")) {
                int end=line.indexOf(": last ");if(end<=0||Character.isWhitespace(line.charAt(0)))continue;
                String name=line.substring(0,end), lower=name.toLowerCase(java.util.Locale.ROOT);
                if((lower.contains("light")||lower.contains("als"))&&!SensorChoice.auxiliary(lower)&&!lower.contains("back")&&!lower.contains("rear")) {
                    if(chosen!=null&&!chosen.equals(name))return null;chosen=name;
                }
            }
            if(chosen==null)return null;sensorName=chosen;
        }
        boolean live=activeOnChange(text,sensorName);
        boolean section=false;Sample latest=null;
        Pattern row=Pattern.compile("^\\s*\\d+ \\(ts=([0-9]+(?:\\.[0-9]+)?), wall=[^)]*\\)\\s+([0-9.eE+\\-]+),?\\s*$");
        for(String line:text.split("\\r?\\n")) {
            if(!line.isEmpty()&&!Character.isWhitespace(line.charAt(0))) {
                section=line.startsWith(sensorName+": last ");continue;
            }
            if(!section)continue;
            Matcher m=row.matcher(line);if(!m.matches())continue;
            try {
                double ts=Double.parseDouble(m.group(1))*1000;
                if(!Double.isFinite(ts)||ts<0||ts>Long.MAX_VALUE)continue;
                Sample sample=new Sample(parseLux(m.group(2)),(long)ts,"dumpsys:"+sensorName,live?now:(long)ts);
                if(sample.usable(now,sessionStart)&&(latest==null||sample.at>latest.at))latest=sample;
            }catch(RuntimeException ignored){}
        }
        return latest;
    }
    // A fresh dump must independently prove exact identity, lux units, on-change
    // reporting and an active HAL subscription. Never renew a lease from cache.
    static boolean activeOnChange(String text,String name) {
        String[] lines=text.split("\\r?\\n");String handle=null;boolean onChange=false;
        Pattern header=Pattern.compile("^(0x[0-9a-fA-F]+)\\)\\s*"+Pattern.quote(name)+"\\s*\\|.*?type:\\s*([^|]+)\\|.*$");
        for(int i=0;i<lines.length;i++) {
            Matcher m=header.matcher(lines[i]);if(!m.matches())continue;
            if(handle!=null)return false;
            handle=m.group(1);String type=m.group(2).trim();
            boolean lux=SensorChoice.luxSensor(type.equals("android.sensor.light(5)")?5:-1,name,type);
            onChange=lux&&i+1<lines.length&&lines[i+1].trim().startsWith("on-change |");
        }
        if(!onChange)return false;
        int device=text.indexOf("Sensor Device:"),list=text.indexOf("Sensor List:");
        if(device<0||list<=device)return false;
        Matcher active=Pattern.compile("(?m)^"+Pattern.quote(handle)+"\\) active-count = ([0-9]+);").matcher(text.substring(device,list));
        if(!active.find())return false;
        try{return Long.parseLong(active.group(1))>0&&!active.find();}catch(NumberFormatException e){return false;}
    }
    public static List<File> candidates(File sensorsRoot, File iioRoot) {
        List<File> found=new ArrayList<>();
        File[] sensors=sensorsRoot.listFiles();
        if(sensors!=null)for(File sensor:sensors) {
            String name=sensor.getName().toLowerCase(java.util.Locale.ROOT);
            if(name.contains("back")||name.contains("rear"))continue;
            File lux=new File(sensor,"lux");if(lux.isFile()&&lux.canRead())found.add(lux);
        }
        File[] devices=iioRoot.listFiles();
        if(devices!=null)for(File device:devices) {
            File lux=new File(device,"in_illuminance_input");
            if(lux.isFile()&&lux.canRead())found.add(lux);
        }
        return found;
    }
}
