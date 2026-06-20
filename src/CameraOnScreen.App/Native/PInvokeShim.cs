using System.Runtime.InteropServices;
using System.Text;
using CameraOnScreen.Core.Native;

namespace CameraOnScreen.App.Native;

public sealed class PInvokeShim : INativeShim
{
    private const string Dll = "CameraOnScreen.Shim.dll";

    [StructLayout(LayoutKind.Sequential)]
    private struct CosStatus
    {
        public int running; public double fps; public int gaze;
        public int green_screen_active; public int eye_contact_active;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string error;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct CosParams
    {
        public IntPtr camera_id;
        public int green_screen_enabled; public double green_screen_strength;
        public int eye_contact_enabled; public double eye_contact_sensitivity;
        public double eye_contact_look_away_range;
    }

    [DllImport(Dll)] private static extern int cos_init(IntPtr device);
    [DllImport(Dll)] private static extern int cos_enumerate_cameras(byte[] ids, byte[] names, int max);
    [DllImport(Dll)] private static extern void cos_set_params(ref CosParams p);
    [DllImport(Dll)] private static extern void cos_start();
    [DllImport(Dll)] private static extern void cos_stop();
    [DllImport(Dll)] private static extern void cos_get_status(out CosStatus s);
    [DllImport(Dll)] private static extern int cos_get_frame(byte[] dst, out int w, out int h, int cap);
    [DllImport(Dll)] private static extern void cos_shutdown();

    public bool Init(IntPtr d3dDevice) => cos_init(d3dDevice) != 0;

    public IReadOnlyList<CameraInfo> EnumerateCameras()
    {
        const int max = 16, stride = 128;
        var ids = new byte[max * stride];
        var names = new byte[max * stride];
        int n = cos_enumerate_cameras(ids, names, max);
        var list = new List<CameraInfo>(n);
        for (int i = 0; i < n; i++)
            list.Add(new CameraInfo(
                ReadUtf8(ids, i * stride, stride),
                ReadUtf8(names, i * stride, stride)));
        return list;
    }

    public void SetParams(ShimParams p)
    {
        // Native side decodes camera_id as UTF-8 (MultiByteToWideChar with CP_UTF8),
        // so marshal UTF-8 here (paired with FreeCoTaskMem below), not ANSI.
        var idPtr = p.CameraId is null ? IntPtr.Zero : Marshal.StringToCoTaskMemUTF8(p.CameraId);
        try
        {
            var native = new CosParams
            {
                camera_id = idPtr,
                green_screen_enabled = p.GreenScreenEnabled ? 1 : 0,
                green_screen_strength = p.GreenScreenStrength,
                eye_contact_enabled = p.EyeContactEnabled ? 1 : 0,
                eye_contact_sensitivity = p.EyeContactSensitivity,
                eye_contact_look_away_range = p.EyeContactLookAwayRange,
            };
            cos_set_params(ref native);
        }
        finally { if (idPtr != IntPtr.Zero) Marshal.FreeCoTaskMem(idPtr); }
    }

    public void Start() => cos_start();
    public void Stop() => cos_stop();

    public ShimStatus GetStatus()
    {
        cos_get_status(out var s);
        return new ShimStatus(
            Running: s.running != 0, Fps: s.fps, Gaze: (GazeState)s.gaze,
            GreenScreenActive: s.green_screen_active != 0,
            EyeContactActive: s.eye_contact_active != 0,
            Error: string.IsNullOrEmpty(s.error) ? null : s.error);
    }

    public bool TryGetFrame(byte[] buffer, out int width, out int height)
        => cos_get_frame(buffer, out width, out height, buffer.Length) != 0;

    public void Dispose() => cos_shutdown();

    private static string ReadUtf8(byte[] buf, int offset, int len)
    {
        int end = offset;
        while (end < offset + len && buf[end] != 0) end++;
        return Encoding.UTF8.GetString(buf, offset, end - offset);
    }
}
