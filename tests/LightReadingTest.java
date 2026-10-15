// SPDX-License-Identifier: GPL-3.0-only
import org.ref1nd.brightness.LightReading;

public final class LightReadingTest {
    static void expect(float expected, float actual) {
        if (Float.compare(expected, actual) != 0)
            throw new AssertionError("Expected " + expected + ", got " + actual);
    }

    public static void main(String[] args) {
        LightReading reading = new LightReading();
        reading.update(12, 1000);
        expect(-1, reading.effective(1000));
        reading.registration(true, true);
        expect(-1, reading.effective(1000)); // Must receive the first event.
        reading.update(12, 1000);
        expect(12, reading.effective(31000)); // Old implementation expired here.
        expect(12, reading.effective(3601000)); // Stable for an hour.
        reading.update(0, 3601000);
        expect(0, reading.effective(7201000)); // Darkness is a valid stable value.
        reading.update(Float.NaN, 7201000);
        reading.update(Float.POSITIVE_INFINITY, 7201000);
        reading.update(-1, 7201000);
        expect(0, reading.effective(7201000));
        if (reading.age(7201000) != 3600000) throw new AssertionError("Invalid event refreshed sample");

        reading.registration(false, false); // Lock screen / manual mode.
        reading.update(99, 7201001); // Ignore callbacks while unregistered.
        expect(-1, reading.raw());
        expect(-1, reading.effective(7201001));
        if (reading.age(7201001) != -1 || reading.isRegistered()) throw new AssertionError("Suspended");
        reading.registration(true, true); // Wake must wait for a new sample.
        expect(-1, reading.effective(7201002));
        reading.update(5, 7201002);
        expect(5, reading.effective(7241002));
        reading.registration(true, true); // Re-register also resets the cache.
        expect(-1, reading.effective(7241002));
        reading.update(8, 7241002);
        reading.registration(false, true); // Failed registration cannot retain data.
        expect(-1, reading.effective(7241003));

        reading.registration(true, false); // Continuous reporting keeps its timeout.
        reading.update(7, 0);
        expect(7, reading.effective(29999));
        expect(-1, reading.effective(30000));
        expect(-1, reading.effective(3600000));
        reading.update(9, 3600000);
        expect(9, reading.effective(3600000));
        expect(-1, reading.effective(3599999));
        System.out.println("Light reading freshness and registration lifecycle passed");
    }
}
