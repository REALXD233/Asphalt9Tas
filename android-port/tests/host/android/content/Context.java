package android.content;

import java.io.File;
import java.lang.reflect.Proxy;
import java.util.HashMap;
import java.util.Map;

/** Host-only filesystem/preferences substitute; never packaged in the APK. */
public class Context {
    public static final int MODE_PRIVATE = 0;
    private final File root;
    private final Map<String, Object> values = new HashMap<>();
    private final SharedPreferences preferences;
    public Context(File root) {
        this.root = root;
        SharedPreferences.Editor editor = (SharedPreferences.Editor) Proxy.newProxyInstance(
                getClass().getClassLoader(), new Class<?>[]{SharedPreferences.Editor.class},
                (proxy, method, args) -> {
                    if (method.getName().startsWith("put")) values.put((String) args[0], args[1]);
                    if (method.getName().equals("remove")) values.remove(args[0]);
                    if (method.getName().equals("commit")) return true;
                    if (method.getName().equals("apply")) return null;
                    return proxy;
                });
        preferences = (SharedPreferences) Proxy.newProxyInstance(
                getClass().getClassLoader(), new Class<?>[]{SharedPreferences.class},
                (proxy, method, args) -> {
                    if (method.getName().equals("edit")) return editor;
                    if (method.getName().equals("getAll")) return new HashMap<>(values);
                    if (method.getName().equals("contains")) return values.containsKey(args[0]);
                    if (method.getName().startsWith("get")) return values.getOrDefault(args[0], args[1]);
                    return null;
                });
    }
    public File getFilesDir() { return root; }
    public SharedPreferences getSharedPreferences(String name, int mode) { return preferences; }
}
