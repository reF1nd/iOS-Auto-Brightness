// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;
import java.util.regex.*;
public final class WindowPolicy {
    /** 0 clear, 1 overridden, -1 unknown; never treat an unrecognized dump as clear. */
    public static int parse(String dump) {
        Matcher m=Pattern.compile("mScreenBrightnessOverrideFromWindowManager=([^\\r\\n]+)").matcher(dump);
        if(!m.find())return -1;
        String value=m.group(1).trim();
        if(value.startsWith("{")) {
            Matcher main=Pattern.compile("(?:\\{|,)\\s*0=([^,}]+)").matcher(value);
            if(!main.find())return -1;value=main.group(1).trim();
        }
        try {float v=Float.parseFloat(value);return Float.isNaN(v)||v<0?0:Float.isFinite(v)&&v<=1?1:-1;}
        catch(Exception e){return -1;}
    }
}
