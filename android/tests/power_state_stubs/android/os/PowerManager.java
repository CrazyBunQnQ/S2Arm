package android.os;
public class PowerManager {
    public int thermal;
    public boolean save;
    public boolean throwThermal, throwSave;
    public int getCurrentThermalStatus() { if (throwThermal) throw new IllegalStateException("thermal"); return thermal; }
    public boolean isPowerSaveMode() { if (throwSave) throw new IllegalStateException("save"); return save; }
}
