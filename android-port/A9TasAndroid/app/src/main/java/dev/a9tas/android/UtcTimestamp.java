package dev.a9tas.android;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.GregorianCalendar;
import java.util.Locale;
import java.util.TimeZone;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** API-24-compatible UTC timestamp codec for the canonical recording manifest. */
final class UtcTimestamp {
    private static final TimeZone UTC = TimeZone.getTimeZone("UTC");
    private static final Pattern FORMAT = Pattern.compile(
            "([0-9]{4})-([0-9]{2})-([0-9]{2})T([0-9]{2}):([0-9]{2}):" +
                    "([0-9]{2})(?:[.]([0-9]{1,9}))?Z");

    private UtcTimestamp() {}

    static String nowSeconds() {
        SimpleDateFormat formatter = new SimpleDateFormat(
                "yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.ROOT);
        formatter.setTimeZone(UTC);
        formatter.setLenient(false);
        return formatter.format(new Date());
    }

    static boolean valid(String value) {
        Matcher match = FORMAT.matcher(value == null ? "" : value);
        if (!match.matches()) return false;
        try {
            GregorianCalendar calendar = new GregorianCalendar(UTC, Locale.ROOT);
            calendar.setLenient(false);
            calendar.clear();
            calendar.set(Integer.parseInt(match.group(1)),
                    Integer.parseInt(match.group(2)) - 1,
                    Integer.parseInt(match.group(3)),
                    Integer.parseInt(match.group(4)),
                    Integer.parseInt(match.group(5)),
                    Integer.parseInt(match.group(6)));
            String fraction = match.group(7);
            if (fraction != null) {
                String millis = (fraction + "000").substring(0, 3);
                calendar.set(GregorianCalendar.MILLISECOND, Integer.parseInt(millis));
            }
            calendar.getTimeInMillis();
            return true;
        } catch (IllegalArgumentException error) {
            return false;
        }
    }

    static void selfTest() {
        String now = nowSeconds();
        if (!valid(now) || !valid("2024-02-29T23:59:59.123456789Z") ||
                valid("2023-02-29T12:00:00Z") || valid("2024-01-01T24:00:00Z") ||
                valid("2024-01-01T00:00:00+00:00"))
            throw new IllegalStateException("API-24 UTC timestamp self-test failed");
    }
}
