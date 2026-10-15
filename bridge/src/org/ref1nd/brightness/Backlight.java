// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

import java.io.*;
import java.util.*;

/** Read-only node diagnostics. Never assumes a node belongs to display 0. */
public final class Backlight {
    public static final class Row {
        public final String path;
        public final long requested, actual, maximum;
        Row(File dir){path=dir.getPath();requested=number(new File(dir,"brightness"));actual=number(new File(dir,"actual_brightness"));maximum=number(new File(dir,"max_brightness"));}
    }
    static long number(File file) {
        try(Reader in=new FileReader(file)) {
            char[] b=new char[64];int n=in.read(b);if(n<=0||n==b.length)return -1;
            long value=Long.parseLong(new String(b,0,n).trim());return value>=0?value:-1;
        }catch(Exception e){return -1;}
    }
    public static List<Row> read(File root) {
        List<Row> rows=new ArrayList<>();File[] dirs=root.listFiles();
        if(dirs!=null){Arrays.sort(dirs);for(File dir:dirs)if(dir.isDirectory())rows.add(new Row(dir));}
        return rows;
    }
}
