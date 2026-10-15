// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;
import java.util.*;
import java.security.MessageDigest;
import java.nio.charset.StandardCharsets;

/** Exact sensor names, no fuzzy matching or raw ADC-to-lux guessing. */
public final class SensorChoice {
    public final String front,back,sysfs;
    public SensorChoice(String text) {
        Map<String,String> values=new HashMap<>();
        for(String row:text.split("\\n")) {
            String s=row.trim();if(s.isEmpty()||s.startsWith("#"))continue;
            int eq=s.indexOf('=');if(eq<0)throw new IllegalArgumentException("expected key=value");
            String k=s.substring(0,eq).trim(),v=s.substring(eq+1).trim();
            if(!Arrays.asList("front","back","sysfs").contains(k)||values.put(k,v)!=null||v.length()>256||v.chars().anyMatch(c->c<32))throw new IllegalArgumentException("invalid sensor selection");
        }
        front=values.getOrDefault("front","");back=values.getOrDefault("back","");sysfs=values.getOrDefault("sysfs","");
        if(!sysfs.isEmpty()&&(!sysfs.startsWith("/sys/")||sysfs.contains("..")||!sysfs.matches("/sys/[a-zA-Z0-9_./:-]+")||!(sysfs.endsWith("/lux")||sysfs.endsWith("/in_illuminance_input"))))throw new IllegalArgumentException("sysfs must be a lux or in_illuminance_input node under /sys");
    }
    public static boolean auxiliary(String name) {
        String n=name.toLowerCase(Locale.ROOT);
        return n.contains("raw")||n.contains("fov");
    }
    public static boolean luxSensor(int type,String name,String stringType) {
        return !auxiliary(name+" "+stringType)&&(type==5||stringType.toLowerCase(Locale.ROOT).contains("lux"));
    }
    public String encode(){return "front="+front+"\nback="+back+"\nsysfs="+sysfs+"\n";}
    public static String scope(String identity) throws Exception {
        byte[] hash=MessageDigest.getInstance("SHA-256").digest(identity.getBytes(StandardCharsets.UTF_8));
        StringBuilder s=new StringBuilder("s");for(int i=0;i<12;i++)s.append(String.format(Locale.ROOT,"%02x",hash[i]&255));return s.toString();
    }
}
