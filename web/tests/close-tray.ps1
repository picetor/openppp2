param([Parameter(Mandatory)][int]$HostProcessId)
$ErrorActionPreference = 'Stop'
# Address only the test-owned host, never an existing user's VPN process.
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class TrayTest {
    public delegate bool EnumProc(IntPtr window, IntPtr state);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr state);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder name, int size);
    [DllImport("user32.dll", EntryPoint = "GetClassLongPtrW")] public static extern IntPtr GetClassLongPtr(IntPtr window, int index);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
    public static void Close(uint expectedPid) {
        IntPtr target = IntPtr.Zero;
        EnumWindows((window, state) => {
            uint pid;
            GetWindowThreadProcessId(window, out pid);
            var name = new StringBuilder(256);
            GetClassName(window, name, name.Capacity);
            if (pid == expectedPid && name.ToString() == "OpenPPP2WebTray") target = window;
            return true;
        }, IntPtr.Zero);
        if (target == IntPtr.Zero) throw new Exception("Test host has no tray window");
        if (GetClassLongPtr(target, -14) == IntPtr.Zero) throw new Exception("Tray window has no embedded project icon");
        if (!PostMessage(target, 0x0010, IntPtr.Zero, IntPtr.Zero)) throw new Exception("Cannot close test tray window");
    }
}
'@
[TrayTest]::Close([uint32]$HostProcessId)
