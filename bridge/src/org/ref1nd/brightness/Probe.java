// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;
import java.lang.reflect.*;
import java.util.*;

/** Read-only framework capability probe. Never changes display state. */
public final class Probe {
    public static void main(String[] args) throws Exception {
        Class.forName("android.os.Looper").getMethod("prepareMainLooper").invoke(null);
        Class<?> at = Class.forName("android.app.ActivityThread");
        Object thread = at.getMethod("systemMain").invoke(null);
        Object context = at.getMethod("getSystemContext").invoke(thread);
        Object display = context.getClass().getMethod("getSystemService", String.class).invoke(context, "display");
        for (String type : new String[]{"android.hardware.display.DisplayManager", "android.hardware.display.DisplayManagerGlobal", "android.hardware.display.IDisplayManager", "android.hardware.display.BrightnessInfo", "android.hardware.display.BrightnessChangeEvent"}) {
            Class<?> cls = Class.forName(type);
            System.out.println("TYPE " + type);
            for (Method m : cls.getDeclaredMethods()) {
                String n = m.getName().toLowerCase(Locale.ROOT);
                if (n.contains("brightness") || n.contains("displayinfo")) System.out.println(m);
            }
            if (type.endsWith("Info") || type.endsWith("Event")) for (Field f : cls.getFields()) System.out.println(f);
        }
        try {
            Object info = display.getClass().getMethod("getBrightnessInfo", int.class).invoke(display, 0);
            System.out.println("BRIGHTNESS_INFO " + info);
        } catch (Exception e) { System.out.println("INFO_ERROR " + e); }
        try {
            Object events = display.getClass().getMethod("getBrightnessEvents").invoke(display);
            System.out.println("EVENTS_TYPE " + events.getClass());
            List<?> list = (List<?>) events;
            System.out.println("EVENT_COUNT " + list.size());
            if (!list.isEmpty()) {
                Object e = list.get(list.size()-1);
                for (String key : new String[]{"timeStamp", "brightness", "userId", "uniqueDisplayId", "luxValues", "isUserSetBrightness"}) {
                    Object v = e.getClass().getField(key).get(e);
                    System.out.println("LAST_EVENT " + key + "=" + (v instanceof float[] ? Arrays.toString((float[]) v) : v));
                }
            }
        } catch (Exception e) { System.out.println("EVENT_ERROR " + e); if (e.getCause()!=null) System.out.println(e.getCause()); }
    }
}
