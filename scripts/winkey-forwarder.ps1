# Focus-scoped Windows-key forwarder - app-shell prototype.
# While the QEMU window is foreground: swallows the Windows key on the host (no Start
# menu, no Win+combos) and forwards it to the guest as Super (meta_l) over QMP.
# While any other window is foreground: the Windows key behaves normally.
# Also keeps the SDL window retitled to "Try Omarchy" - users must never see QEMU
# chrome (QEMU resets its title on every grab toggle, so this reasserts periodically).
# Pair with SDL_GRAB_KEYBOARD=0 so SDL never installs its own (system-wide) hook.
#   powershell -ExecutionPolicy Bypass -File winkey-forwarder.ps1 [-QmpPort 4446]
param([int]$QmpPort = 4446)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class WinKeyForwarder
{
    delegate IntPtr HookProc(int nCode, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] static extern IntPtr SetWindowsHookEx(int id, HookProc proc, IntPtr hMod, uint tid);
    [DllImport("user32.dll")] static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] static extern int GetMessage(out MSG m, IntPtr h, uint mi, uint ma);
    [DllImport("kernel32.dll")] static extern IntPtr GetModuleHandle(string name);
    [StructLayout(LayoutKind.Sequential)]
    struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam; public IntPtr lParam; public uint time; public int ptX; public int ptY; }

    const int WH_KEYBOARD_LL = 13;
    const int WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_SYSKEYDOWN = 0x104, WM_SYSKEYUP = 0x105;
    const int VK_LWIN = 0x5B, VK_RWIN = 0x5C;

    static HookProc keep;   // prevent GC of the delegate
    static ConcurrentQueue<bool> queue = new ConcurrentQueue<bool>();
    static AutoResetEvent kick = new AutoResetEvent(false);
    static bool winDown = false;
    static volatile int qemuPid = -1;
    static int qmpPort = 4446;

    static IntPtr Callback(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode >= 0)
        {
            int msg = (int)wParam;
            int vk = Marshal.ReadInt32(lParam);
            if (vk == VK_LWIN || vk == VK_RWIN)
            {
                bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
                if (ForegroundIsQemu())
                {
                    if (down != winDown) { winDown = down; queue.Enqueue(down); kick.Set(); }
                    return (IntPtr)1;   // swallow on host, deliver to guest via QMP
                }
                if (winDown) { winDown = false; queue.Enqueue(false); kick.Set(); }  // focus left mid-press
                // VM not focused: approve the key and SKIP the rest of the hook chain.
                // QEMU installs its own LL hook on every grab which swallows Win even
                // when unfocused; returning 0 without CallNextHookEx bypasses it.
                return IntPtr.Zero;
            }
        }
        return CallNextHookEx(IntPtr.Zero, nCode, wParam, lParam);
    }

    static bool ForegroundIsQemu()
    {
        uint pid; GetWindowThreadProcessId(GetForegroundWindow(), out pid);
        return pid != 0 && pid == (uint)qemuPid;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool SetWindowText(IntPtr hWnd, string text);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr hWnd, StringBuilder buf, int max);
    const string AppTitle = "Try Omarchy";

    static void PidRefresher()
    {
        while (true)
        {
            try
            {
                Process[] ps = Process.GetProcessesByName("qemu-system-x86_64");
                qemuPid = ps.Length > 0 ? ps[0].Id : -1;
                if (ps.Length > 0)
                {
                    IntPtr hw = ps[0].MainWindowHandle;
                    if (hw != IntPtr.Zero)
                    {
                        StringBuilder sb = new StringBuilder(256);
                        GetWindowText(hw, sb, 256);
                        if (sb.ToString() != AppTitle) SetWindowText(hw, AppTitle);
                    }
                }
                foreach (Process p in ps) p.Dispose();
            }
            catch { }
            Thread.Sleep(3000);
        }
    }

    static void QmpWorker()
    {
        while (true)
        {
            try
            {
                using (TcpClient tcp = new TcpClient("127.0.0.1", qmpPort))
                {
                    NetworkStream s = tcp.GetStream();
                    StreamWriter w = new StreamWriter(s); w.AutoFlush = true;
                    StreamReader r = new StreamReader(s);
                    r.ReadLine();
                    w.WriteLine("{\"execute\":\"qmp_capabilities\"}");
                    r.ReadLine();
                    Console.WriteLine("QMP connected on port " + qmpPort);
                    while (tcp.Connected)
                    {
                        kick.WaitOne(1000);
                        bool d;
                        while (queue.TryDequeue(out d))
                        {
                            w.WriteLine("{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"key\",\"data\":{\"down\":" + (d ? "true" : "false") + ",\"key\":{\"type\":\"qcode\",\"data\":\"meta_l\"}}}]}}");
                            r.ReadLine();
                        }
                    }
                }
            }
            catch { Thread.Sleep(2000); }
        }
    }

    [DllImport("user32.dll")] static extern bool UnhookWindowsHookEx(IntPtr hhk);
    [DllImport("user32.dll")] static extern uint MsgWaitForMultipleObjects(uint n, IntPtr[] handles, bool all, uint ms, uint mask);
    [DllImport("user32.dll")] static extern bool PeekMessage(out MSG m, IntPtr h, uint mi, uint ma, uint remove);
    const uint QS_ALLINPUT = 0x04FF, PM_REMOVE = 1;

    public static void Run(int port)
    {
        qmpPort = port;
        Thread t1 = new Thread(PidRefresher); t1.IsBackground = true; t1.Start();
        Thread t2 = new Thread(QmpWorker); t2.IsBackground = true; t2.Start();
        keep = Callback;
        IntPtr h = SetWindowsHookEx(WH_KEYBOARD_LL, keep, GetModuleHandle(null), 0);
        if (h == IntPtr.Zero) throw new Exception("SetWindowsHookEx failed");
        Console.WriteLine("winkey-forwarder active: Super -> Omarchy when the VM window is focused");
        // QEMU re-installs its own LL hook on every grab; LL hooks run newest-first.
        // Re-hook periodically so this hook stays at the front of the chain.
        MSG m;
        while (true)
        {
            MsgWaitForMultipleObjects(0, null, false, 800, QS_ALLINPUT);
            while (PeekMessage(out m, IntPtr.Zero, 0, 0, PM_REMOVE)) { }
            UnhookWindowsHookEx(h);
            h = SetWindowsHookEx(WH_KEYBOARD_LL, keep, GetModuleHandle(null), 0);
            if (h == IntPtr.Zero) { Thread.Sleep(500); h = SetWindowsHookEx(WH_KEYBOARD_LL, keep, GetModuleHandle(null), 0); }
        }
    }
}
"@

[WinKeyForwarder]::Run($QmpPort)
