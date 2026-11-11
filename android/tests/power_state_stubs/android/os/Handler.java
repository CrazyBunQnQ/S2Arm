package android.os;
public class Handler {
    public Runnable pending;
    public long lastDelay = -1;
    public boolean post(Runnable task) { pending = task; lastDelay = 0; return true; }
    public boolean postDelayed(Runnable task, long delay) { pending = task; lastDelay = delay; return true; }
    public void removeCallbacks(Runnable task) { if (pending == task) pending = null; }
    public void runPending() { Runnable task = pending; pending = null; if (task != null) task.run(); }
}
