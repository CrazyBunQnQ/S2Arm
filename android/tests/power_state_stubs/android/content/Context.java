package android.content;
public class Context {
    public android.os.PowerManager power = new android.os.PowerManager();
    @SuppressWarnings("unchecked")
    public <T> T getSystemService(Class<T> type) { return type.isInstance(power) ? (T) power : null; }
}
