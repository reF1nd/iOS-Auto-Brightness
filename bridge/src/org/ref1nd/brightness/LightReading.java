// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

/** One light reading, valid only within its current registration. */
public final class LightReading {
    private static final long TIMEOUT_MS = 30000;
    private boolean registered, onChange;
    private float lux = -1;
    private long sampledAt = -1;

    public void registration(boolean succeeded, boolean reportsOnChange) {
        registered = succeeded;
        onChange = reportsOnChange;
        lux = -1;
        sampledAt = -1;
    }

    public void update(float value, long now) {
        if (!registered || !Float.isFinite(value) || value < 0 || now < 0) return;
        lux = value;
        sampledAt = now;
    }

    public long age(long now) {
        return registered && sampledAt >= 0 && now >= sampledAt ? now - sampledAt : -1;
    }

    public float effective(long now) {
        long age = age(now);
        // On-change sensors need not repeat a stable value, including zero.
        return age >= 0 && (onChange || age < TIMEOUT_MS) ? lux : -1;
    }

    public float raw() { return lux; }
    public boolean isRegistered() { return registered; }
}
