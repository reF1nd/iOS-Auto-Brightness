// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;
import java.util.Locale;
public final class ThermalPolicy {
    public static boolean boostSource(String name) {
        String n=name.toLowerCase(Locale.ROOT);
        // v6.8 includes SoC readings when gating sunlight boost.
        if(n.contains("soc"))return true;
        if(n.contains("cpu")||n.contains("gpu"))return false;
        return n.contains("battery")||n.contains("batt")||n.contains("skin")||n.contains("shell")||n.contains("quiet")||n.contains("back");
    }
}
