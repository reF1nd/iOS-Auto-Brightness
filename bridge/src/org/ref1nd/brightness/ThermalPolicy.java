// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;
import java.util.Locale;
public final class ThermalPolicy {
    public static boolean surface(String name) {
        String n=name.toLowerCase(Locale.ROOT);
        // Never apply a skin-temperature threshold to CPU/SoC/GPU junctions.
        if(n.contains("soc")||n.contains("cpu")||n.contains("gpu"))return false;
        return n.contains("battery")||n.contains("batt")||n.contains("skin")||n.contains("shell")||n.contains("quiet")||n.contains("back");
    }
}
