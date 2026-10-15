// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.regex.*;

/** Only accepts calibration tied to the selected physical display. */
public final class PanelProfile {
    public final float[] nits, brightness;
    public final String id;
    public final boolean calibrated;
    PanelProfile(float[] n,float[] b,String identity,boolean calibrated) throws Exception {
        if(n.length<2||n.length>256||n.length!=b.length)throw new IllegalArgumentException("calibration length");
        StringBuilder key=new StringBuilder(identity);
        for(int i=0;i<n.length;i++) {
            if(!Float.isFinite(n[i])||!Float.isFinite(b[i])||n[i]<0||n[i]>20000||b[i]<0||b[i]>1||
                (i>0&&(n[i]<=n[i-1]||b[i]<=b[i-1])))throw new IllegalArgumentException("calibration range/order");
            key.append(':').append(n[i]).append(':').append(b[i]);
        }
        this.nits=n;this.brightness=b;this.calibrated=calibrated;
        byte[] digest=MessageDigest.getInstance("SHA-256").digest(key.toString().getBytes(StandardCharsets.UTF_8));
        StringBuilder hash=new StringBuilder();for(int i=0;i<12;i++)hash.append(String.format(java.util.Locale.ROOT,"%02x",digest[i]));
        id=(calibrated?"panel-":"generic-")+hash;
    }
    static float[] array(String segment,String name) {
        Matcher m=Pattern.compile(name+"=\\[([^\\]]+)\\]").matcher(segment);
        if(!m.find())throw new IllegalArgumentException("missing "+name);
        String[] values=m.group(1).split(",");float[] result=new float[values.length];
        for(int i=0;i<values.length;i++)result[i]=Float.parseFloat(values[i].trim());
        return result;
    }
    public static PanelProfile fromDump(String dump,String identity) throws Exception {
        int at=dump.indexOf("uniqueId=\""+identity+"\"");
        if(at<0)throw new IllegalArgumentException("display identity missing");
        int end=dump.indexOf("DisplayDeviceInfo{",at);
        String segment=dump.substring(at,end<0?dump.length():end);
        // Framework brightness and HAL backlight are not interchangeable.
        return new PanelProfile(array(segment,"mNits"),array(segment,"mBrightness"),identity,true);
    }
    public static PanelProfile generic(String identity) throws Exception {
        return new PanelProfile(new float[]{0,1,5,20,100,300,538.5f},new float[]{0,.01f,.025f,.06f,.2f,.55f,1},identity,false);
    }
    public boolean matches(String text) {
        try(java.util.Scanner s=new java.util.Scanner(text).useLocale(java.util.Locale.ROOT)) {
            if(!s.next().equals("IAB_PROFILE_V1"))return false;s.next();if(s.nextInt()!=nits.length)return false;
            for(int i=0;i<nits.length;i++)if(Math.abs(s.nextFloat()-nits[i])>.001||Math.abs(s.nextFloat()-brightness[i])>.000001)return false;
            return !s.hasNext();
        }catch(Exception e){return false;}
    }
    public String encode(String name) {
        StringBuilder s=new StringBuilder("IAB_PROFILE_V1 ").append(name).append(' ').append(nits.length).append('\n');
        for(int i=0;i<nits.length;i++)s.append(nits[i]).append(' ').append(brightness[i]).append('\n');
        return s.toString();
    }
}
