// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;
import java.io.File;
import org.json.JSONObject;
import android.os.SystemClock;

public final class Health {
    static String identity(long pid,String token) {
        if(pid<=0)return "missing";
        try {
            String cmd=Bridge.read(new File("/proc/"+pid+"/cmdline"),8192);
            for(String arg:cmd.split("\u0000"))if(arg.equals(token))return "alive";
            return "identity_mismatch";
        }catch(Exception e){return "dead_or_unreadable";}
    }
    static long processStart(long pid) {
        try {
            String stat=Bridge.read(new File("/proc/"+pid+"/stat"),8192);
            return Long.parseLong(stat.substring(stat.lastIndexOf(')')+2).split(" +")[19]);
        }catch(Exception e){return -1;}
    }
    static String checkedProcess(JSONObject s,String key,String token) {
        long pid=s.optLong(key+"_pid",-1),start=s.optLong(key+"_start",-1);
        if(start<0||processStart(pid)!=start)return "dead_or_reused_pid";
        return identity(pid,token);
    }
    public static String freshness(long now,long heartbeat,boolean sameBoot) {
        if(!sameBoot)return "previous_boot";
        if(heartbeat<0||heartbeat>now)return "invalid_clock";
        return now-heartbeat>20000?"stale":"fresh";
    }
    public static JSONObject report(File state,File module) throws Exception {
        JSONObject out=new JSONObject().put("ok",true);long supervisor=-1;
        try{supervisor=Long.parseLong(Bridge.read(new File(state,"supervisor.pid"),64).trim());}catch(Exception ignored){}
        // Supervisor may execute from the staged update directory until the next boot.
        String active="/data/adb/modules/ios_auto_brightness/service.sh",staged="/data/adb/modules_update/ios_auto_brightness/service.sh";
        String sup=identity(supervisor,active);if(!sup.equals("alive"))sup=identity(supervisor,staged);
        out.put("supervisor",sup);
        try {
            JSONObject s=new JSONObject(Bridge.read(new File(state,"status.json"),131072));
            String boot=Bridge.read(new File("/proc/sys/kernel/random/boot_id"),128).trim();
            out.put("heartbeat",freshness(SystemClock.elapsedRealtime(),s.optLong("heartbeat_ms",-1),boot.equals(s.optString("boot_id"))))
                .put("bridge_process",checkedProcess(s,"bridge","org.ref1nd.brightness.Bridge"))
                .put("core_process",checkedProcess(s,"core",new File(module,"bin/ios_brightness_daemon").getPath()))
                .put("bridge_state",s.optString("bridge","unknown"));
            if(!out.optString("core_process").equals("alive"))out.put("core_process",checkedProcess(s,"core","/data/adb/modules_update/ios_auto_brightness/bin/ios_brightness_daemon"));
        }catch(Exception e){out.put("snapshot","missing_or_invalid");}
        boolean healthy=sup.equals("alive")&&out.optString("heartbeat").equals("fresh")&&out.optString("bridge_process").equals("alive")&&out.optString("core_process").equals("alive")&&out.optString("bridge_state").equals("active");
        return out.put("health",healthy?"healthy":"attention_required");
    }
}
