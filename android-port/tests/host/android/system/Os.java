package android.system;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
public final class Os {
    public static void rename(String from, String to) throws ErrnoException {
        try { Files.move(Path.of(from), Path.of(to), StandardCopyOption.REPLACE_EXISTING); }
        catch (Exception error) { throw new AssertionError(error); }
    }
}
