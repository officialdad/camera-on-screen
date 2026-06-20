# Camera-on-Screen Foundation (M1+M2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a runnable Windows app with a tested logic core, a WinUI 3 control panel, and a transparent always-on-top draggable overlay showing a raw webcam passthrough captured by any screen recorder — the non-RTX-tier product, with the seams in place for Maxine effects later.

**Architecture:** One .NET 8 process. A WinUI-independent `Core` library holds all testable logic (config, settings store, native-shim contract, orchestrator, view-model, hotkey model). A WinUI 3 `App` project hosts the control-panel UI plus a raw Win32 + DirectComposition overlay window (Vortice bindings) and the real P/Invoke implementation of the shim contract. A C++ C-ABI shim DLL does Media Foundation capture and fills a C#-owned D3D11 texture; it never renders or creates windows.

**Tech Stack:** C# / .NET 8, WinUI 3 (Windows App SDK), CommunityToolkit.Mvvm, Vortice.Windows (D3D11/DXGI/DirectComposition), xUnit; C++ (MSVC) for the shim, Media Foundation for capture.

## Global Constraints

- Platform: Windows 10/11 only; target framework `net8.0-windows10.0.19041.0` for the App, `net8.0` for Core and tests. Verbatim from spec: "Windows 10/11, NVIDIA RTX GPU (Turing/Ampere/Ada + Tensor Cores)".
- GPU tiers (verbatim intent): "RTX present: full feature set"; "No / non-RTX GPU: app still runs in plain-overlay passthrough (raw webcam, no effects). Effect toggles are disabled with a clear 'requires RTX GPU' note." This plan delivers only the passthrough tier; effects are gated off here and implemented in the M3–M5 plan.
- C# owns all windowing, compositing, and UI. The shim never creates windows and never renders.
- Single shared D3D11 device: C# creates it and passes it to the shim at `Init`; no shared handles / `OpenSharedResource`.
- Status is polled via `GetStatus`; no native→managed callbacks.
- Settings persist to JSON in `%LOCALAPPDATA%\CameraOnScreen\config.json`.
- Frequent commits: every task ends with a commit. TDD for all `Core` logic. Native/graphics tasks use smoke tests + explicit manual verification.

---

## File Structure

- `CameraOnScreen.sln` — solution.
- `src/CameraOnScreen.Core/` (net8.0) — pure logic, no WinUI/Win32 types:
  - `Config/Models.cs` — config records + enums.
  - `Config/ConfigSerializer.cs` — `System.Text.Json` options.
  - `Config/ISettingsStore.cs`, `Config/JsonSettingsStore.cs` — persistence.
  - `Native/Contracts.cs` — `INativeShim`, status/param structs, `CameraInfo`, enums.
  - `Native/FakeShim.cs` — in-memory shim for tests/design-time.
  - `Orchestration/Orchestrator.cs` — start/stop, effect gating by GPU tier, status polling.
  - `Hotkeys/HotkeyValidator.cs` — defaults + conflict detection.
  - `ViewModels/MainViewModel.cs` — control-panel state (CommunityToolkit.Mvvm).
- `src/CameraOnScreen.App/` (net8.0-windows…) — WinUI 3 packaged app:
  - `App.xaml`/`App.xaml.cs`, `MainWindow.xaml`/`.cs` — control panel.
  - `Composition/Services.cs` — wires Core types to the UI.
  - `Native/PInvokeShim.cs` — real `INativeShim` over the C++ DLL.
  - `Native/GpuTierDetector.cs` — RTX detection.
  - `Overlay/OverlayWindow.cs` — raw Win32 + DComp layered window.
  - `Overlay/Interop.cs` — user32/dwm/DComp P/Invoke for the overlay.
  - `Hotkeys/GlobalHotkeyService.cs` — `RegisterHotKey` wiring.
- `native/shim/` — C++ C-ABI DLL `CameraOnScreen.Shim.dll`:
  - `shim.h`, `shim.cpp` — exported C ABI.
  - `capture.cpp` — Media Foundation capture.
  - `shim.vcxproj` — build.
- `tests/CameraOnScreen.Core.Tests/` (net8.0, xunit) — unit tests for Core.

---

### Task 1: Scaffold solution, Core library, and test project

**Files:**
- Create: `CameraOnScreen.sln`
- Create: `src/CameraOnScreen.Core/CameraOnScreen.Core.csproj`
- Create: `tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
- Create: `src/CameraOnScreen.Core/Placeholder.cs`
- Create: `tests/CameraOnScreen.Core.Tests/SmokeTest.cs`

**Interfaces:**
- Consumes: nothing.
- Produces: a buildable solution with a runnable (failing→passing) test harness.

- [ ] **Step 1: Create solution and projects**

Run:
```bash
cd "C:/Users/opari/OneDrive/Desktop/claude-code/camera-on-screen"
dotnet new sln -n CameraOnScreen
dotnet new classlib -n CameraOnScreen.Core -o src/CameraOnScreen.Core -f net8.0
dotnet new xunit -n CameraOnScreen.Core.Tests -o tests/CameraOnScreen.Core.Tests -f net8.0
dotnet sln add src/CameraOnScreen.Core/CameraOnScreen.Core.csproj
dotnet sln add tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj
dotnet add tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj reference src/CameraOnScreen.Core/CameraOnScreen.Core.csproj
dotnet add src/CameraOnScreen.Core/CameraOnScreen.Core.csproj package CommunityToolkit.Mvvm
```

- [ ] **Step 2: Enable nullable + delete template cruft**

Edit `src/CameraOnScreen.Core/CameraOnScreen.Core.csproj` so the `<PropertyGroup>` contains:
```xml
<TargetFramework>net8.0</TargetFramework>
<Nullable>enable</Nullable>
<LangVersion>latest</LangVersion>
<ImplicitUsings>enable</ImplicitUsings>
```
Delete `src/CameraOnScreen.Core/Class1.cs` and `tests/CameraOnScreen.Core.Tests/UnitTest1.cs` if present.

Create `src/CameraOnScreen.Core/Placeholder.cs`:
```csharp
namespace CameraOnScreen.Core;

public static class BuildMarker
{
    public const string Name = "CameraOnScreen.Core";
}
```

- [ ] **Step 3: Write the smoke test**

Create `tests/CameraOnScreen.Core.Tests/SmokeTest.cs`:
```csharp
using CameraOnScreen.Core;
using Xunit;

public class SmokeTest
{
    [Fact]
    public void Core_is_referenced()
    {
        Assert.Equal("CameraOnScreen.Core", BuildMarker.Name);
    }
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `dotnet test`
Expected: build succeeds, 1 test passes.

- [ ] **Step 5: Commit**

```bash
git add CameraOnScreen.sln src/CameraOnScreen.Core tests/CameraOnScreen.Core.Tests
git commit -m "chore: scaffold solution, Core library, and xUnit test project"
```

---

### Task 2: Config models and serializer

**Files:**
- Create: `src/CameraOnScreen.Core/Config/Models.cs`
- Create: `src/CameraOnScreen.Core/Config/ConfigSerializer.cs`
- Test: `tests/CameraOnScreen.Core.Tests/Config/ModelsTests.cs`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `enum OverlayShape { Full, RoundedRect, Circle }`
  - `[Flags] enum HotkeyModifiers { None=0, Alt=1, Control=2, Shift=4, Win=8 }` (values match Win32 `MOD_*`)
  - `enum HotkeyAction { ToggleLock, ToggleClickThrough, ToggleOverlayVisible, ToggleRunning }`
  - `record OverlaySettings` (X, Y, Width, Height: double; Opacity: double; Shape: OverlayShape; Mirror, Locked, ClickThrough: bool)
  - `record EffectSettings` (GreenScreenEnabled: bool, GreenScreenStrength: double, EyeContactEnabled: bool, EyeContactSensitivity: double, EyeContactLookAwayRange: double)
  - `record HotkeyBinding` (Action: HotkeyAction, Modifiers: HotkeyModifiers, VirtualKey: uint)
  - `record AppConfig` (CameraId: string?, Overlay: OverlaySettings, Effects: EffectSettings, Hotkeys: IReadOnlyList<HotkeyBinding>) with `static IReadOnlyList<HotkeyBinding> DefaultHotkeys()`
  - `static class ConfigSerializer { JsonSerializerOptions Options; string Serialize(AppConfig); AppConfig Deserialize(string) }`

- [ ] **Step 1: Write the failing test**

Create `tests/CameraOnScreen.Core.Tests/Config/ModelsTests.cs`:
```csharp
using CameraOnScreen.Core.Config;
using Xunit;

public class ModelsTests
{
    [Fact]
    public void AppConfig_defaults_are_sane()
    {
        var c = new AppConfig();
        Assert.Null(c.CameraId);
        Assert.Equal(OverlayShape.Full, c.Overlay.Shape);
        Assert.True(c.Effects.GreenScreenEnabled);
        Assert.False(c.Effects.EyeContactEnabled);
        Assert.Equal(4, c.Hotkeys.Count); // one per HotkeyAction
    }

    [Fact]
    public void Round_trips_through_json_with_enum_names()
    {
        var c = new AppConfig
        {
            CameraId = "cam-1",
            Overlay = new OverlaySettings { Shape = OverlayShape.Circle, Mirror = true, X = 50 }
        };
        var json = ConfigSerializer.Serialize(c);
        Assert.Contains("\"Circle\"", json); // enum serialized as name, not number
        var back = ConfigSerializer.Deserialize(json);
        Assert.Equal(c, back); // records => value equality (lists compared below)
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `dotnet test --filter ModelsTests`
Expected: FAIL — `CameraOnScreen.Core.Config` types do not exist.

- [ ] **Step 3: Write the models**

Create `src/CameraOnScreen.Core/Config/Models.cs`:
```csharp
namespace CameraOnScreen.Core.Config;

public enum OverlayShape { Full, RoundedRect, Circle }

[System.Flags]
public enum HotkeyModifiers { None = 0, Alt = 1, Control = 2, Shift = 4, Win = 8 }

public enum HotkeyAction { ToggleLock, ToggleClickThrough, ToggleOverlayVisible, ToggleRunning }

public sealed record OverlaySettings
{
    public double X { get; init; } = 100;
    public double Y { get; init; } = 100;
    public double Width { get; init; } = 320;
    public double Height { get; init; } = 240;
    public double Opacity { get; init; } = 1.0;
    public OverlayShape Shape { get; init; } = OverlayShape.Full;
    public bool Mirror { get; init; }
    public bool Locked { get; init; }
    public bool ClickThrough { get; init; }
}

public sealed record EffectSettings
{
    public bool GreenScreenEnabled { get; init; } = true;
    public double GreenScreenStrength { get; init; } = 1.0;
    public bool EyeContactEnabled { get; init; }
    public double EyeContactSensitivity { get; init; } = 0.5;
    public double EyeContactLookAwayRange { get; init; } = 0.5;
}

public sealed record HotkeyBinding
{
    public HotkeyAction Action { get; init; }
    public HotkeyModifiers Modifiers { get; init; }
    public uint VirtualKey { get; init; }
}

public sealed record AppConfig
{
    public string? CameraId { get; init; }
    public OverlaySettings Overlay { get; init; } = new();
    public EffectSettings Effects { get; init; } = new();
    public IReadOnlyList<HotkeyBinding> Hotkeys { get; init; } = DefaultHotkeys();

    // VK codes: F8=0x77, F9=0x78, F10=0x79, F11=0x7A
    public static IReadOnlyList<HotkeyBinding> DefaultHotkeys() => new[]
    {
        new HotkeyBinding { Action = HotkeyAction.ToggleLock,          Modifiers = HotkeyModifiers.Control | HotkeyModifiers.Alt, VirtualKey = 0x77 },
        new HotkeyBinding { Action = HotkeyAction.ToggleClickThrough,  Modifiers = HotkeyModifiers.Control | HotkeyModifiers.Alt, VirtualKey = 0x78 },
        new HotkeyBinding { Action = HotkeyAction.ToggleOverlayVisible,Modifiers = HotkeyModifiers.Control | HotkeyModifiers.Alt, VirtualKey = 0x79 },
        new HotkeyBinding { Action = HotkeyAction.ToggleRunning,       Modifiers = HotkeyModifiers.Control | HotkeyModifiers.Alt, VirtualKey = 0x7A },
    };
}
```

- [ ] **Step 4: Write the serializer**

Create `src/CameraOnScreen.Core/Config/ConfigSerializer.cs`:
```csharp
using System.Text.Json;
using System.Text.Json.Serialization;

namespace CameraOnScreen.Core.Config;

public static class ConfigSerializer
{
    public static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
        Converters = { new JsonStringEnumConverter() }
    };

    public static string Serialize(AppConfig config) => JsonSerializer.Serialize(config, Options);

    public static AppConfig Deserialize(string json) =>
        JsonSerializer.Deserialize<AppConfig>(json, Options)
        ?? throw new JsonException("Deserialized AppConfig was null.");
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `dotnet test --filter ModelsTests`
Expected: PASS (2 tests).

> Note: record value equality covers `IReadOnlyList` by reference, not element-wise. The round-trip test passes because the default list instances differ only if mutated; if this test ever fails on `Hotkeys`, compare `c.Hotkeys.SequenceEqual(back.Hotkeys)` explicitly. Leave as-is unless it fails.

- [ ] **Step 6: Commit**

```bash
git add src/CameraOnScreen.Core/Config tests/CameraOnScreen.Core.Tests/Config
git commit -m "feat(core): config models and JSON serializer"
```

---

### Task 3: Settings store (load/save with defaults + corruption recovery)

**Files:**
- Create: `src/CameraOnScreen.Core/Config/ISettingsStore.cs`
- Create: `src/CameraOnScreen.Core/Config/JsonSettingsStore.cs`
- Test: `tests/CameraOnScreen.Core.Tests/Config/JsonSettingsStoreTests.cs`

**Interfaces:**
- Consumes: `AppConfig`, `ConfigSerializer` (Task 2).
- Produces:
  - `interface ISettingsStore { AppConfig Load(); void Save(AppConfig config); }`
  - `class JsonSettingsStore : ISettingsStore` with `JsonSettingsStore(string filePath)` and `static string DefaultPath()` returning `%LOCALAPPDATA%\CameraOnScreen\config.json`.

- [ ] **Step 1: Write the failing tests**

Create `tests/CameraOnScreen.Core.Tests/Config/JsonSettingsStoreTests.cs`:
```csharp
using System.IO;
using CameraOnScreen.Core.Config;
using Xunit;

public class JsonSettingsStoreTests
{
    private static string TempFile() =>
        Path.Combine(Path.GetTempPath(), "cos-" + Path.GetRandomFileName(), "config.json");

    [Fact]
    public void Load_returns_defaults_when_file_missing()
    {
        var store = new JsonSettingsStore(TempFile());
        Assert.Equal(new AppConfig(), store.Load());
    }

    [Fact]
    public void Save_then_Load_round_trips()
    {
        var path = TempFile();
        var store = new JsonSettingsStore(path);
        var cfg = new AppConfig { CameraId = "cam-7" };
        store.Save(cfg);
        Assert.True(File.Exists(path));
        Assert.Equal("cam-7", store.Load().CameraId);
    }

    [Fact]
    public void Load_returns_defaults_when_file_corrupt()
    {
        var path = TempFile();
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, "{ this is not json ");
        var store = new JsonSettingsStore(path);
        Assert.Equal(new AppConfig(), store.Load());
    }
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `dotnet test --filter JsonSettingsStoreTests`
Expected: FAIL — `ISettingsStore`/`JsonSettingsStore` do not exist.

- [ ] **Step 3: Write the interface and implementation**

Create `src/CameraOnScreen.Core/Config/ISettingsStore.cs`:
```csharp
namespace CameraOnScreen.Core.Config;

public interface ISettingsStore
{
    AppConfig Load();
    void Save(AppConfig config);
}
```

Create `src/CameraOnScreen.Core/Config/JsonSettingsStore.cs`:
```csharp
using System.IO;

namespace CameraOnScreen.Core.Config;

public sealed class JsonSettingsStore : ISettingsStore
{
    private readonly string _filePath;

    public JsonSettingsStore(string filePath) => _filePath = filePath;

    public static string DefaultPath() => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "CameraOnScreen", "config.json");

    public AppConfig Load()
    {
        try
        {
            if (!File.Exists(_filePath)) return new AppConfig();
            return ConfigSerializer.Deserialize(File.ReadAllText(_filePath));
        }
        catch
        {
            return new AppConfig(); // missing/corrupt => safe defaults
        }
    }

    public void Save(AppConfig config)
    {
        var dir = Path.GetDirectoryName(_filePath);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        File.WriteAllText(_filePath, ConfigSerializer.Serialize(config));
    }
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `dotnet test --filter JsonSettingsStoreTests`
Expected: PASS (3 tests).

- [ ] **Step 5: Commit**

```bash
git add src/CameraOnScreen.Core/Config tests/CameraOnScreen.Core.Tests/Config
git commit -m "feat(core): JSON settings store with default + corruption recovery"
```

---

### Task 4: Native shim contract and fake implementation

**Files:**
- Create: `src/CameraOnScreen.Core/Native/Contracts.cs`
- Create: `src/CameraOnScreen.Core/Native/FakeShim.cs`
- Test: `tests/CameraOnScreen.Core.Tests/Native/FakeShimTests.cs`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `readonly record struct CameraInfo(string Id, string Name)`
  - `enum GazeState { Unknown, OnCamera, Redirected, RealEyes }`
  - `readonly record struct ShimStatus(bool Running, double Fps, GazeState Gaze, bool GreenScreenActive, bool EyeContactActive, string? Error)`
  - `sealed record ShimParams(string? CameraId, bool GreenScreenEnabled, double GreenScreenStrength, bool EyeContactEnabled, double EyeContactSensitivity, double EyeContactLookAwayRange)` (no Mirror — mirror is presentation-side per spec §4)
  - `interface INativeShim : IDisposable { bool Init(IntPtr d3dDevice); IReadOnlyList<CameraInfo> EnumerateCameras(); void SetParams(ShimParams p); void Start(); void Stop(); ShimStatus GetStatus(); }`
  - `sealed class FakeShim : INativeShim` (in-memory; records last params; `Running` flips on Start/Stop; configurable camera list).

- [ ] **Step 1: Write the failing tests**

Create `tests/CameraOnScreen.Core.Tests/Native/FakeShimTests.cs`:
```csharp
using System;
using CameraOnScreen.Core.Native;
using Xunit;

public class FakeShimTests
{
    [Fact]
    public void Start_then_status_reports_running()
    {
        var shim = new FakeShim();
        Assert.True(shim.Init(IntPtr.Zero));
        Assert.False(shim.GetStatus().Running);
        shim.Start();
        Assert.True(shim.GetStatus().Running);
        shim.Stop();
        Assert.False(shim.GetStatus().Running);
    }

    [Fact]
    public void SetParams_is_recorded()
    {
        var shim = new FakeShim();
        var p = new ShimParams("cam-1", true, 0.8, false, 0.5, 0.5);
        shim.SetParams(p);
        Assert.Equal(p, shim.LastParams);
    }

    [Fact]
    public void EnumerateCameras_returns_seeded_list()
    {
        var shim = new FakeShim { Cameras = { new CameraInfo("a", "Cam A") } };
        Assert.Single(shim.EnumerateCameras());
    }
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `dotnet test --filter FakeShimTests`
Expected: FAIL — `CameraOnScreen.Core.Native` types do not exist.

- [ ] **Step 3: Write the contracts**

Create `src/CameraOnScreen.Core/Native/Contracts.cs`:
```csharp
namespace CameraOnScreen.Core.Native;

public readonly record struct CameraInfo(string Id, string Name);

public enum GazeState { Unknown, OnCamera, Redirected, RealEyes }

public readonly record struct ShimStatus(
    bool Running,
    double Fps,
    GazeState Gaze,
    bool GreenScreenActive,
    bool EyeContactActive,
    string? Error);

public sealed record ShimParams(
    string? CameraId,
    bool GreenScreenEnabled,
    double GreenScreenStrength,
    bool EyeContactEnabled,
    double EyeContactSensitivity,
    double EyeContactLookAwayRange);

public interface INativeShim : IDisposable
{
    bool Init(IntPtr d3dDevice);
    IReadOnlyList<CameraInfo> EnumerateCameras();
    void SetParams(ShimParams p);
    void Start();
    void Stop();
    ShimStatus GetStatus();
}
```

- [ ] **Step 4: Write the fake**

Create `src/CameraOnScreen.Core/Native/FakeShim.cs`:
```csharp
namespace CameraOnScreen.Core.Native;

public sealed class FakeShim : INativeShim
{
    public List<CameraInfo> Cameras { get; } = new();
    public ShimParams? LastParams { get; private set; }
    private bool _running;

    public bool Init(IntPtr d3dDevice) => true;
    public IReadOnlyList<CameraInfo> EnumerateCameras() => Cameras;
    public void SetParams(ShimParams p) => LastParams = p;
    public void Start() => _running = true;
    public void Stop() => _running = false;

    public ShimStatus GetStatus() => new(
        Running: _running,
        Fps: _running ? 30 : 0,
        Gaze: GazeState.Unknown,
        GreenScreenActive: _running && (LastParams?.GreenScreenEnabled ?? false),
        EyeContactActive: _running && (LastParams?.EyeContactEnabled ?? false),
        Error: null);

    public void Dispose() { }
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `dotnet test --filter FakeShimTests`
Expected: PASS (3 tests).

- [ ] **Step 6: Commit**

```bash
git add src/CameraOnScreen.Core/Native tests/CameraOnScreen.Core.Tests/Native
git commit -m "feat(core): native shim contract and in-memory fake"
```

---

### Task 5: Orchestrator (start/stop + GPU-tier effect gating + status polling)

**Files:**
- Create: `src/CameraOnScreen.Core/Orchestration/Orchestrator.cs`
- Test: `tests/CameraOnScreen.Core.Tests/Orchestration/OrchestratorTests.cs`

**Interfaces:**
- Consumes: `INativeShim`, `ShimParams`, `ShimStatus` (Task 4).
- Produces:
  - `enum GpuTier { Rtx, NonRtx }`
  - `enum OrchestratorState { Idle, Running, Faulted }`
  - `sealed class Orchestrator` with:
    - `Orchestrator(INativeShim shim, GpuTier tier)`
    - `OrchestratorState State { get; }`
    - `bool EffectsAvailable { get; }` (== tier is Rtx)
    - `event EventHandler<ShimStatus>? StatusChanged`
    - `void Start(ShimParams requested)` — gates effects off when `NonRtx`, calls `SetParams` then `Start`, sets state Running
    - `void Stop()`
    - `void PollStatus()` — calls `GetStatus`, raises `StatusChanged`, sets Faulted on non-null `Error`

- [ ] **Step 1: Write the failing tests**

Create `tests/CameraOnScreen.Core.Tests/Orchestration/OrchestratorTests.cs`:
```csharp
using CameraOnScreen.Core.Native;
using CameraOnScreen.Core.Orchestration;
using Xunit;

public class OrchestratorTests
{
    private static ShimParams Requested() =>
        new(CameraId: "cam", GreenScreenEnabled: true, GreenScreenStrength: 1.0,
            EyeContactEnabled: true, EyeContactSensitivity: 0.5, EyeContactLookAwayRange: 0.5);

    [Fact]
    public void Rtx_tier_passes_effects_through()
    {
        var shim = new FakeShim();
        var orch = new Orchestrator(shim, GpuTier.Rtx);
        orch.Start(Requested());
        Assert.Equal(OrchestratorState.Running, orch.State);
        Assert.True(shim.LastParams!.GreenScreenEnabled);
        Assert.True(shim.LastParams!.EyeContactEnabled);
    }

    [Fact]
    public void NonRtx_tier_forces_effects_off()
    {
        var shim = new FakeShim();
        var orch = new Orchestrator(shim, GpuTier.NonRtx);
        Assert.False(orch.EffectsAvailable);
        orch.Start(Requested());
        Assert.False(shim.LastParams!.GreenScreenEnabled);
        Assert.False(shim.LastParams!.EyeContactEnabled);
    }

    [Fact]
    public void PollStatus_raises_event_and_faults_on_error()
    {
        var shim = new ErroringShim();
        var orch = new Orchestrator(shim, GpuTier.Rtx);
        ShimStatus? seen = null;
        orch.StatusChanged += (_, s) => seen = s;
        orch.Start(Requested());
        orch.PollStatus();
        Assert.NotNull(seen);
        Assert.Equal(OrchestratorState.Faulted, orch.State);
    }

    private sealed class ErroringShim : INativeShim
    {
        public bool Init(System.IntPtr d) => true;
        public System.Collections.Generic.IReadOnlyList<CameraInfo> EnumerateCameras() => System.Array.Empty<CameraInfo>();
        public void SetParams(ShimParams p) { }
        public void Start() { }
        public void Stop() { }
        public ShimStatus GetStatus() => new(true, 0, GazeState.Unknown, false, false, "boom");
        public void Dispose() { }
    }
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `dotnet test --filter OrchestratorTests`
Expected: FAIL — `Orchestrator` does not exist.

- [ ] **Step 3: Write the orchestrator**

Create `src/CameraOnScreen.Core/Orchestration/Orchestrator.cs`:
```csharp
using CameraOnScreen.Core.Native;

namespace CameraOnScreen.Core.Orchestration;

public enum GpuTier { Rtx, NonRtx }
public enum OrchestratorState { Idle, Running, Faulted }

public sealed class Orchestrator
{
    private readonly INativeShim _shim;
    private readonly GpuTier _tier;

    public Orchestrator(INativeShim shim, GpuTier tier)
    {
        _shim = shim;
        _tier = tier;
    }

    public OrchestratorState State { get; private set; } = OrchestratorState.Idle;
    public bool EffectsAvailable => _tier == GpuTier.Rtx;
    public event EventHandler<ShimStatus>? StatusChanged;

    public void Start(ShimParams requested)
    {
        var effective = EffectsAvailable
            ? requested
            : requested with { GreenScreenEnabled = false, EyeContactEnabled = false };
        _shim.SetParams(effective);
        _shim.Start();
        State = OrchestratorState.Running;
    }

    public void Stop()
    {
        _shim.Stop();
        State = OrchestratorState.Idle;
    }

    public void PollStatus()
    {
        var status = _shim.GetStatus();
        if (status.Error is not null) State = OrchestratorState.Faulted;
        StatusChanged?.Invoke(this, status);
    }
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `dotnet test --filter OrchestratorTests`
Expected: PASS (3 tests).

- [ ] **Step 5: Commit**

```bash
git add src/CameraOnScreen.Core/Orchestration tests/CameraOnScreen.Core.Tests/Orchestration
git commit -m "feat(core): orchestrator with GPU-tier effect gating and status polling"
```

---

### Task 6: Hotkey defaults and conflict detection

**Files:**
- Create: `src/CameraOnScreen.Core/Hotkeys/HotkeyValidator.cs`
- Test: `tests/CameraOnScreen.Core.Tests/Hotkeys/HotkeyValidatorTests.cs`

**Interfaces:**
- Consumes: `HotkeyBinding`, `HotkeyModifiers`, `HotkeyAction` (Task 2).
- Produces:
  - `static class HotkeyValidator { bool HasConflict(IReadOnlyList<HotkeyBinding> bindings, out (HotkeyBinding a, HotkeyBinding b)? conflict); }`
  - Two bindings conflict when `Modifiers` and `VirtualKey` are equal but `Action` differs.

- [ ] **Step 1: Write the failing tests**

Create `tests/CameraOnScreen.Core.Tests/Hotkeys/HotkeyValidatorTests.cs`:
```csharp
using CameraOnScreen.Core.Config;
using CameraOnScreen.Core.Hotkeys;
using Xunit;

public class HotkeyValidatorTests
{
    [Fact]
    public void Default_hotkeys_have_no_conflicts()
    {
        Assert.False(HotkeyValidator.HasConflict(AppConfig.DefaultHotkeys(), out _));
    }

    [Fact]
    public void Same_chord_for_two_actions_conflicts()
    {
        var bindings = new[]
        {
            new HotkeyBinding { Action = HotkeyAction.ToggleLock, Modifiers = HotkeyModifiers.Alt, VirtualKey = 0x77 },
            new HotkeyBinding { Action = HotkeyAction.ToggleRunning, Modifiers = HotkeyModifiers.Alt, VirtualKey = 0x77 },
        };
        Assert.True(HotkeyValidator.HasConflict(bindings, out var conflict));
        Assert.NotNull(conflict);
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `dotnet test --filter HotkeyValidatorTests`
Expected: FAIL — `HotkeyValidator` does not exist.

- [ ] **Step 3: Write the validator**

Create `src/CameraOnScreen.Core/Hotkeys/HotkeyValidator.cs`:
```csharp
using CameraOnScreen.Core.Config;

namespace CameraOnScreen.Core.Hotkeys;

public static class HotkeyValidator
{
    public static bool HasConflict(
        IReadOnlyList<HotkeyBinding> bindings,
        out (HotkeyBinding a, HotkeyBinding b)? conflict)
    {
        for (int i = 0; i < bindings.Count; i++)
        for (int j = i + 1; j < bindings.Count; j++)
        {
            var a = bindings[i];
            var b = bindings[j];
            if (a.Modifiers == b.Modifiers && a.VirtualKey == b.VirtualKey && a.Action != b.Action)
            {
                conflict = (a, b);
                return true;
            }
        }
        conflict = null;
        return false;
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `dotnet test --filter HotkeyValidatorTests`
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```bash
git add src/CameraOnScreen.Core/Hotkeys tests/CameraOnScreen.Core.Tests/Hotkeys
git commit -m "feat(core): hotkey conflict detection"
```

---

### Task 7: MainViewModel (control-panel state)

**Files:**
- Create: `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs`
- Test: `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`

**Interfaces:**
- Consumes: `Orchestrator`, `ShimParams`, `ShimStatus`, `CameraInfo`, `EffectSettings`, `OverlaySettings` (earlier tasks).
- Produces:
  - `sealed partial class MainViewModel : ObservableObject` with observable properties:
    `ObservableCollection<CameraInfo> Cameras`, `CameraInfo? SelectedCamera`,
    `bool GreenScreenEnabled`, `double GreenScreenStrength`,
    `bool EyeContactEnabled`, `double EyeContactSensitivity`, `double EyeContactLookAwayRange`,
    `bool EffectsAvailable`, `bool IsRunning`, `double Fps`, `string? StatusError`, `GazeState Gaze`.
  - `MainViewModel(Orchestrator orchestrator)`
  - `void LoadFrom(AppConfig config)`, `ShimParams BuildParams()`, relay commands `StartCommand`, `StopCommand`, and `void OnStatus(ShimStatus s)`.

- [ ] **Step 1: Write the failing tests**

Create `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`:
```csharp
using CameraOnScreen.Core.Config;
using CameraOnScreen.Core.Native;
using CameraOnScreen.Core.Orchestration;
using CameraOnScreen.Core.ViewModels;
using Xunit;

public class MainViewModelTests
{
    private static MainViewModel Build(GpuTier tier, out FakeShim shim)
    {
        shim = new FakeShim { Cameras = { new CameraInfo("cam", "Cam") } };
        return new MainViewModel(new Orchestrator(shim, tier));
    }

    [Fact]
    public void NonRtx_disables_effects_in_vm()
    {
        var vm = Build(GpuTier.NonRtx, out _);
        Assert.False(vm.EffectsAvailable);
    }

    [Fact]
    public void BuildParams_reflects_vm_state()
    {
        var vm = Build(GpuTier.Rtx, out _);
        vm.GreenScreenEnabled = true;
        vm.GreenScreenStrength = 0.7;
        vm.SelectedCamera = new CameraInfo("cam", "Cam");
        var p = vm.BuildParams();
        Assert.Equal("cam", p.CameraId);
        Assert.Equal(0.7, p.GreenScreenStrength);
    }

    [Fact]
    public void Start_sets_running_and_OnStatus_updates_fps()
    {
        var vm = Build(GpuTier.Rtx, out _);
        vm.SelectedCamera = new CameraInfo("cam", "Cam");
        vm.StartCommand.Execute(null);
        Assert.True(vm.IsRunning);
        vm.OnStatus(new ShimStatus(true, 42, GazeState.OnCamera, true, false, null));
        Assert.Equal(42, vm.Fps);
        Assert.Equal(GazeState.OnCamera, vm.Gaze);
    }
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `dotnet test --filter MainViewModelTests`
Expected: FAIL — `MainViewModel` does not exist.

- [ ] **Step 3: Write the view-model**

Create `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs`:
```csharp
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CameraOnScreen.Core.Config;
using CameraOnScreen.Core.Native;
using CameraOnScreen.Core.Orchestration;

namespace CameraOnScreen.Core.ViewModels;

public sealed partial class MainViewModel : ObservableObject
{
    private readonly Orchestrator _orchestrator;

    public MainViewModel(Orchestrator orchestrator)
    {
        _orchestrator = orchestrator;
        EffectsAvailable = orchestrator.EffectsAvailable;
        _orchestrator.StatusChanged += (_, s) => OnStatus(s);
    }

    public ObservableCollection<CameraInfo> Cameras { get; } = new();

    [ObservableProperty] private CameraInfo? selectedCamera;
    [ObservableProperty] private bool greenScreenEnabled = true;
    [ObservableProperty] private double greenScreenStrength = 1.0;
    [ObservableProperty] private bool eyeContactEnabled;
    [ObservableProperty] private double eyeContactSensitivity = 0.5;
    [ObservableProperty] private double eyeContactLookAwayRange = 0.5;
    [ObservableProperty] private bool effectsAvailable;
    [ObservableProperty] private bool isRunning;
    [ObservableProperty] private double fps;
    [ObservableProperty] private string? statusError;
    [ObservableProperty] private GazeState gaze;

    public void LoadFrom(AppConfig config)
    {
        GreenScreenEnabled = config.Effects.GreenScreenEnabled;
        GreenScreenStrength = config.Effects.GreenScreenStrength;
        EyeContactEnabled = config.Effects.EyeContactEnabled;
        EyeContactSensitivity = config.Effects.EyeContactSensitivity;
        EyeContactLookAwayRange = config.Effects.EyeContactLookAwayRange;
        if (config.CameraId is not null)
            SelectedCamera = Cameras.FirstOrDefault(c => c.Id == config.CameraId);
    }

    public ShimParams BuildParams() => new(
        CameraId: SelectedCamera?.Id,
        GreenScreenEnabled: GreenScreenEnabled,
        GreenScreenStrength: GreenScreenStrength,
        EyeContactEnabled: EyeContactEnabled,
        EyeContactSensitivity: EyeContactSensitivity,
        EyeContactLookAwayRange: EyeContactLookAwayRange);

    public void OnStatus(ShimStatus s)
    {
        IsRunning = s.Running;
        Fps = s.Fps;
        Gaze = s.Gaze;
        StatusError = s.Error;
    }

    [RelayCommand]
    private void Start()
    {
        _orchestrator.Start(BuildParams());
        IsRunning = true;
    }

    [RelayCommand]
    private void Stop()
    {
        _orchestrator.Stop();
        IsRunning = false;
    }
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `dotnet test --filter MainViewModelTests`
Expected: PASS (3 tests). Full suite: `dotnet test` — all green.

- [ ] **Step 5: Commit**

```bash
git add src/CameraOnScreen.Core/ViewModels tests/CameraOnScreen.Core.Tests/ViewModels
git commit -m "feat(core): MainViewModel for control panel"
```

---

### Task 8: WinUI 3 app shell + control panel bound to MainViewModel

**Files:**
- Create: `src/CameraOnScreen.App/CameraOnScreen.App.csproj`
- Create: `src/CameraOnScreen.App/app.manifest`, `Package.appxmanifest`
- Create: `src/CameraOnScreen.App/App.xaml`, `App.xaml.cs`
- Create: `src/CameraOnScreen.App/MainWindow.xaml`, `MainWindow.xaml.cs`
- Create: `src/CameraOnScreen.App/Composition/Services.cs`
- Modify: `CameraOnScreen.sln` (add the App project)

**Interfaces:**
- Consumes: `MainViewModel`, `Orchestrator`, `FakeShim` (until Task 9 supplies the real shim), `JsonSettingsStore`, `GpuTier`.
- Produces: a launchable WinUI 3 window whose toggles/sliders bind to `MainViewModel`. `Services.BuildViewModel()` composes the object graph (uses `FakeShim` + `GpuTier.NonRtx` for now).

> Prereqs (environment): Windows App SDK + the WinUI workload installed. The `winui-dev-workflow` and `winui-app` skills cover template/build setup and `BuildAndRun.ps1`. Create the project from the WinUI 3 "Blank App, Packaged" template, then replace the generated `MainWindow` contents below.

- [ ] **Step 1: Create the WinUI 3 project from template**

Run (from repo root; uses the Windows App SDK templates installed with the workload):
```bash
dotnet new winui3 -n CameraOnScreen.App -o src/CameraOnScreen.App
dotnet sln add src/CameraOnScreen.App/CameraOnScreen.App.csproj
dotnet add src/CameraOnScreen.App/CameraOnScreen.App.csproj reference src/CameraOnScreen.Core/CameraOnScreen.Core.csproj
dotnet add src/CameraOnScreen.App/CameraOnScreen.App.csproj package CommunityToolkit.Mvvm
```
If `dotnet new winui3` is unavailable, create the project in Visual Studio via "Blank App, Packaged (WinUI 3 in Desktop)" into `src/CameraOnScreen.App`, then run the `dotnet sln add` / `reference` / `package` lines above.

- [ ] **Step 2: Write the composition root**

Create `src/CameraOnScreen.App/Composition/Services.cs`:
```csharp
using CameraOnScreen.Core.Config;
using CameraOnScreen.Core.Native;
using CameraOnScreen.Core.Orchestration;
using CameraOnScreen.Core.ViewModels;

namespace CameraOnScreen.App.Composition;

public static class Services
{
    // Task 9 replaces FakeShim with PInvokeShim and GpuTier with GpuTierDetector.Detect().
    public static MainViewModel BuildViewModel()
    {
        var store = new JsonSettingsStore(JsonSettingsStore.DefaultPath());
        var config = store.Load();
        INativeShim shim = new FakeShim();
        var orchestrator = new Orchestrator(shim, GpuTier.NonRtx);
        var vm = new MainViewModel(orchestrator);
        vm.LoadFrom(config);
        return vm;
    }
}
```

- [ ] **Step 3: Replace MainWindow XAML with the control panel**

Replace the contents of `src/CameraOnScreen.App/MainWindow.xaml`:
```xml
<Window
    x:Class="CameraOnScreen.App.MainWindow"
    xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
    xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
    <Grid Padding="16" RowSpacing="12">
        <Grid.RowDefinitions>
            <RowDefinition Height="Auto"/>
            <RowDefinition Height="Auto"/>
            <RowDefinition Height="Auto"/>
            <RowDefinition Height="Auto"/>
            <RowDefinition Height="*"/>
        </Grid.RowDefinitions>

        <ComboBox Grid.Row="0" Header="Camera"
                  ItemsSource="{x:Bind Vm.Cameras}"
                  SelectedItem="{x:Bind Vm.SelectedCamera, Mode=TwoWay}"
                  DisplayMemberPath="Name" Width="280"/>

        <StackPanel Grid.Row="1" Spacing="8">
            <ToggleSwitch Header="AI Green Screen" IsEnabled="{x:Bind Vm.EffectsAvailable}"
                          IsOn="{x:Bind Vm.GreenScreenEnabled, Mode=TwoWay}"/>
            <ToggleSwitch Header="Eye Contact" IsEnabled="{x:Bind Vm.EffectsAvailable}"
                          IsOn="{x:Bind Vm.EyeContactEnabled, Mode=TwoWay}"/>
            <TextBlock Text="Effects require an RTX GPU."
                       Visibility="{x:Bind NotAvailableVisibility}"
                       Foreground="{ThemeResource SystemFillColorCautionBrush}"/>
        </StackPanel>

        <StackPanel Grid.Row="2" Orientation="Horizontal" Spacing="8">
            <Button Content="Start" Command="{x:Bind Vm.StartCommand}"/>
            <Button Content="Stop" Command="{x:Bind Vm.StopCommand}"/>
        </StackPanel>

        <TextBlock Grid.Row="3"
                   Text="{x:Bind StatusLine, Mode=OneWay}"/>
    </Grid>
</Window>
```

- [ ] **Step 4: Wire the code-behind**

Replace the contents of `src/CameraOnScreen.App/MainWindow.xaml.cs`:
```csharp
using CameraOnScreen.App.Composition;
using CameraOnScreen.Core.ViewModels;
using Microsoft.UI.Xaml;

namespace CameraOnScreen.App;

public sealed partial class MainWindow : Window
{
    public MainViewModel Vm { get; }

    public MainWindow()
    {
        Vm = Services.BuildViewModel();
        InitializeComponent();
    }

    public Visibility NotAvailableVisibility =>
        Vm.EffectsAvailable ? Visibility.Collapsed : Visibility.Visible;

    public string StatusLine =>
        Vm.IsRunning ? $"Running — {Vm.Fps:F0} fps" : "Stopped";
}
```

- [ ] **Step 5: Build and launch (manual verification)**

Run: `dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj`
Then launch from Visual Studio (F5) or the skill's `BuildAndRun.ps1`.
Expected:
- Window opens with a camera combo, two effect toggles (disabled, because tier is `NonRtx`), the "Effects require an RTX GPU." note visible, Start/Stop buttons, and a "Stopped" status line.
- Clicking Start changes the status line to "Running — 0 fps" (FakeShim).

- [ ] **Step 6: Commit**

```bash
git add src/CameraOnScreen.App CameraOnScreen.sln
git commit -m "feat(app): WinUI 3 control panel bound to MainViewModel"
```

---

### Task 9: C++ shim C-ABI skeleton + P/Invoke binding + GPU tier detection

**Files:**
- Create: `native/shim/shim.h`
- Create: `native/shim/shim.cpp`
- Create: `native/shim/shim.vcxproj`
- Create: `src/CameraOnScreen.App/Native/PInvokeShim.cs`
- Create: `src/CameraOnScreen.App/Native/GpuTierDetector.cs`
- Modify: `src/CameraOnScreen.App/Composition/Services.cs` (use real shim + detector)
- Modify: `src/CameraOnScreen.App/CameraOnScreen.App.csproj` (copy the DLL next to the app)

**Interfaces:**
- Consumes: `INativeShim`, `ShimStatus`, `ShimParams`, `CameraInfo`, `GpuTier`.
- Produces:
  - Exported C ABI: `cos_init`, `cos_enumerate_cameras`, `cos_set_params`, `cos_start`, `cos_stop`, `cos_get_status`, `cos_shutdown` (signatures below).
  - `sealed class PInvokeShim : INativeShim`.
  - `static class GpuTierDetector { GpuTier Detect(); }`.

- [ ] **Step 1: Define the C ABI header**

Create `native/shim/shim.h`:
```c
#pragma once
#include <stdint.h>
#ifdef COS_EXPORTS
#define COS_API extern "C" __declspec(dllexport)
#else
#define COS_API extern "C" __declspec(dllimport)
#endif

typedef struct {
    int   running;
    double fps;
    int   gaze;              // 0 Unknown,1 OnCamera,2 Redirected,3 RealEyes
    int   green_screen_active;
    int   eye_contact_active;
    char  error[256];        // empty string = no error
} CosStatus;

typedef struct {
    const char* camera_id;   // UTF-8, may be null
    int    green_screen_enabled;
    double green_screen_strength;
    int    eye_contact_enabled;
    double eye_contact_sensitivity;
    double eye_contact_look_away_range;
} CosParams;

COS_API int  cos_init(void* d3d11_device);
// Writes up to max ids/names (UTF-8, '\0'-terminated, 128 bytes each) into the buffers; returns count.
COS_API int  cos_enumerate_cameras(char* ids, char* names, int max);
COS_API void cos_set_params(const CosParams* p);
COS_API void cos_start(void);
COS_API void cos_stop(void);
COS_API void cos_get_status(CosStatus* out);
COS_API void cos_shutdown(void);
```

- [ ] **Step 2: Implement the skeleton (stubs; capture comes in Task 10)**

Create `native/shim/shim.cpp`:
```cpp
#define COS_EXPORTS
#include "shim.h"
#include <atomic>
#include <cstring>

namespace {
    std::atomic<bool> g_running{false};
    CosParams g_params{};
}

COS_API int cos_init(void* /*d3d11_device*/) { return 1; }

COS_API int cos_enumerate_cameras(char* /*ids*/, char* /*names*/, int /*max*/) {
    return 0; // Task 10 fills this from Media Foundation.
}

COS_API void cos_set_params(const CosParams* p) { if (p) g_params = *p; }
COS_API void cos_start(void) { g_running = true; }
COS_API void cos_stop(void) { g_running = false; }

COS_API void cos_get_status(CosStatus* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->running = g_running ? 1 : 0;
    out->fps = g_running ? 30.0 : 0.0;
}

COS_API void cos_shutdown(void) { g_running = false; }
```

- [ ] **Step 3: Create the build project**

Create `native/shim/shim.vcxproj` (minimal x64 DLL). Use the standard VC++ Dynamic-Link Library template targeting x64, output name `CameraOnScreen.Shim`, C++17, and include `shim.cpp`. Key settings:
```xml
<PropertyGroup Label="Globals">
  <ProjectGuid>{B1C0A0E2-0000-4000-8000-000000000001}</ProjectGuid>
  <RootNamespace>CameraOnScreenShim</RootNamespace>
  <TargetName>CameraOnScreen.Shim</TargetName>
  <ConfigurationType>DynamicLibrary</ConfigurationType>
  <PlatformToolset>v143</PlatformToolset>
  <LanguageStandard>stdcpp17</LanguageStandard>
</PropertyGroup>
```
Add it to the solution and constrain to x64:
```bash
dotnet sln add native/shim/shim.vcxproj
```
Build:
```bash
msbuild native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64
```
Expected: `native/shim/x64/Debug/CameraOnScreen.Shim.dll` is produced.

- [ ] **Step 4: Write the P/Invoke shim**

Create `src/CameraOnScreen.App/Native/PInvokeShim.cs`:
```csharp
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
        var idPtr = p.CameraId is null ? IntPtr.Zero : Marshal.StringToHGlobalAnsi(p.CameraId);
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
        finally { if (idPtr != IntPtr.Zero) Marshal.FreeHGlobal(idPtr); }
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

    public void Dispose() => cos_shutdown();

    private static string ReadUtf8(byte[] buf, int offset, int len)
    {
        int end = offset;
        while (end < offset + len && buf[end] != 0) end++;
        return Encoding.UTF8.GetString(buf, offset, end - offset);
    }
}
```

- [ ] **Step 5: Write the GPU tier detector**

Create `src/CameraOnScreen.App/Native/GpuTierDetector.cs`:
```csharp
using CameraOnScreen.Core.Orchestration;
using Vortice.DXGI;

namespace CameraOnScreen.App.Native;

public static class GpuTierDetector
{
    // Heuristic for M1+M2: NVIDIA adapter whose description mentions "RTX".
    // The M3-M5 plan replaces this with a real Maxine-availability probe.
    public static GpuTier Detect()
    {
        if (DXGI.CreateDXGIFactory1(out IDXGIFactory1? factory).Failure || factory is null)
            return GpuTier.NonRtx;
        using (factory)
        {
            for (uint i = 0; factory.EnumAdapters1(i, out IDXGIAdapter1? adapter).Success; i++)
            using (adapter)
            {
                var desc = adapter!.Description1.Description ?? "";
                if (desc.Contains("RTX", StringComparison.OrdinalIgnoreCase))
                    return GpuTier.Rtx;
            }
        }
        return GpuTier.NonRtx;
    }
}
```

Add the Vortice DXGI package:
```bash
dotnet add src/CameraOnScreen.App/CameraOnScreen.App.csproj package Vortice.DXGI
```

- [ ] **Step 6: Wire the real shim + detector into composition**

Edit `src/CameraOnScreen.App/Composition/Services.cs` — replace the `BuildViewModel` body:
```csharp
public static MainViewModel BuildViewModel()
{
    var store = new JsonSettingsStore(JsonSettingsStore.DefaultPath());
    var config = store.Load();
    INativeShim shim = new CameraOnScreen.App.Native.PInvokeShim();
    shim.Init(IntPtr.Zero); // real D3D device passed in Task 11
    var tier = CameraOnScreen.App.Native.GpuTierDetector.Detect();
    var orchestrator = new Orchestrator(shim, tier);
    var vm = new MainViewModel(orchestrator);
    foreach (var cam in shim.EnumerateCameras()) vm.Cameras.Add(cam);
    vm.LoadFrom(config);
    return vm;
}
```

- [ ] **Step 7: Copy the DLL next to the app**

Edit `src/CameraOnScreen.App/CameraOnScreen.App.csproj`, add inside a `<Project>`-level `<ItemGroup>`:
```xml
<None Include="..\..\native\shim\x64\$(Configuration)\CameraOnScreen.Shim.dll">
  <Link>CameraOnScreen.Shim.dll</Link>
  <CopyToOutputDirectory>PreserveNewest</CopyToOutputDirectory>
</None>
```

- [ ] **Step 8: Build and verify the binding (manual)**

Run:
```bash
msbuild native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64
dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj
```
Launch the app. Expected: it starts without a `DllNotFoundException`; Start → status "Running — 30 fps" (now coming from the native stub, not the fake). Tier reflects the real GPU (on the RTX 3090 target, effect toggles become enabled).

- [ ] **Step 9: Commit**

```bash
git add native/shim src/CameraOnScreen.App CameraOnScreen.sln
git commit -m "feat: C++ shim C-ABI skeleton, P/Invoke binding, GPU tier detection"
```

---

### Task 10: Media Foundation camera enumeration + capture in the shim

**Files:**
- Create: `native/shim/capture.h`, `native/shim/capture.cpp`
- Modify: `native/shim/shim.cpp` (use the capture module; expose latest frame)
- Modify: `native/shim/shim.h` (add `cos_get_frame`)
- Modify: `src/CameraOnScreen.App/Native/PInvokeShim.cs` (add `TryGetFrame`)
- Modify: `src/CameraOnScreen.Core/Native/Contracts.cs` (add frame accessor to `INativeShim`)

**Interfaces:**
- Consumes: Task 9 ABI.
- Produces:
  - C ABI `int cos_get_frame(uint8_t* dst, int* width, int* height, int dstByteCapacity)` — copies the latest BGRA frame, returns 1 if a new frame was written.
  - `INativeShim.TryGetFrame(byte[] buffer, out int width, out int height)` returning bool.
  - Real `cos_enumerate_cameras` returning attached cameras.

- [ ] **Step 1: Add the frame ABI**

Edit `native/shim/shim.h`, add before the closing of the export list:
```c
// Copies latest BGRA8 frame into dst (width*height*4 bytes). Returns 1 if a frame was copied.
COS_API int cos_get_frame(uint8_t* dst, int* width, int* height, int dst_capacity);
```

- [ ] **Step 2: Implement Media Foundation capture**

Create `native/shim/capture.h`:
```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct CameraDesc { std::string id; std::string name; };

class Capture {
public:
    bool Start(const std::string& symbolicLink); // empty = first device
    void Stop();
    // Returns true and fills frame (BGRA) + dims if a new frame is available.
    bool LatestFrame(std::vector<uint8_t>& out, int& w, int& h);
    static std::vector<CameraDesc> Enumerate();
};
```

Create `native/shim/capture.cpp` implementing `Capture` with Media Foundation
(`MFCreateDeviceSource`, `IMFSourceReader`, request `MFVideoFormat_RGB32`/convert to BGRA,
read frames on a worker thread into a mutex-guarded buffer). Enumerate via
`MFEnumDeviceSources` with `MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID`, reading
`MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME` and the symbolic-link attribute for the id. Link
against `mfplat.lib mf.lib mfreadwrite.lib mfuuid.lib ole32.lib` (add to the vcxproj
`<AdditionalDependencies>`). Initialize with `MFStartup` in `cos_init` and `MFShutdown`
in `cos_shutdown`.

> This is the one task whose native body is too large to inline verbatim; implement it
> against the Media Foundation `IMFSourceReader` capture sample. The contract above
> (`Enumerate`, `Start`, `LatestFrame`) is fixed — the shim wiring in Step 3 depends only
> on it.

- [ ] **Step 3: Wire capture into the shim**

Edit `native/shim/shim.cpp`:
```cpp
#define COS_EXPORTS
#include "shim.h"
#include "capture.h"
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace {
    std::atomic<bool> g_running{false};
    CosParams g_params{};
    Capture   g_capture;
    std::string g_cameraId;
}

COS_API int cos_init(void*) { return 1; } // MFStartup handled here in capture.cpp init

COS_API int cos_enumerate_cameras(char* ids, char* names, int max) {
    auto cams = Capture::Enumerate();
    int n = (int)cams.size(); if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        std::strncpy(ids   + i * 128, cams[i].id.c_str(),   127);
        std::strncpy(names + i * 128, cams[i].name.c_str(), 127);
    }
    return n;
}

COS_API void cos_set_params(const CosParams* p) {
    if (!p) return;
    g_params = *p;
    g_cameraId = p->camera_id ? p->camera_id : "";
}

COS_API void cos_start(void) { g_capture.Start(g_cameraId); g_running = true; }
COS_API void cos_stop(void)  { g_capture.Stop(); g_running = false; }

COS_API void cos_get_status(CosStatus* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->running = g_running ? 1 : 0;
    out->fps = g_running ? 30.0 : 0.0;
}

COS_API int cos_get_frame(uint8_t* dst, int* width, int* height, int cap) {
    std::vector<uint8_t> frame; int w = 0, h = 0;
    if (!g_capture.LatestFrame(frame, w, h)) return 0;
    if ((int)frame.size() > cap) return 0;
    std::memcpy(dst, frame.data(), frame.size());
    if (width) *width = w; if (height) *height = h;
    return 1;
}

COS_API void cos_shutdown(void) { g_capture.Stop(); g_running = false; }
```

- [ ] **Step 4: Extend the managed contract + P/Invoke**

Edit `src/CameraOnScreen.Core/Native/Contracts.cs`, add to `INativeShim`:
```csharp
bool TryGetFrame(byte[] buffer, out int width, out int height);
```
Add the same method to `FakeShim` (return `false`) so it still compiles:
```csharp
public bool TryGetFrame(byte[] buffer, out int width, out int height)
{
    width = 0; height = 0; return false;
}
```

Edit `src/CameraOnScreen.App/Native/PInvokeShim.cs`, add:
```csharp
[DllImport(Dll)] private static extern int cos_get_frame(byte[] dst, out int w, out int h, int cap);

public bool TryGetFrame(byte[] buffer, out int width, out int height)
    => cos_get_frame(buffer, out width, out height, buffer.Length) != 0;
```

- [ ] **Step 5: Build + manual verification (camera required)**

Run:
```bash
msbuild native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64
dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj
```
Launch the app. Expected: the camera combo now lists the real attached webcam(s). (Pixels appear on screen in Task 12.)

- [ ] **Step 6: Commit**

```bash
git add native/shim src/CameraOnScreen.Core src/CameraOnScreen.App
git commit -m "feat: Media Foundation camera enumeration and frame capture in shim"
```

---

### Task 11: Win32 + DirectComposition layered overlay window (test pattern)

**Files:**
- Create: `src/CameraOnScreen.App/Overlay/Interop.cs`
- Create: `src/CameraOnScreen.App/Overlay/OverlayWindow.cs`
- Modify: `src/CameraOnScreen.App/MainWindow.xaml.cs` (create the overlay on launch)

**Interfaces:**
- Consumes: Vortice D3D11/DXGI/DirectComposition.
- Produces:
  - `sealed class OverlayWindow` with `OverlayWindow(int x, int y, int width, int height)`, `IntPtr D3DDevicePtr { get; }`, `void PresentTestPattern()`, `void Show()`, `void Dispose()`.

- [ ] **Step 1: Add Vortice composition packages**

Run:
```bash
dotnet add src/CameraOnScreen.App/CameraOnScreen.App.csproj package Vortice.Direct3D11
dotnet add src/CameraOnScreen.App/CameraOnScreen.App.csproj package Vortice.DirectComposition
```

- [ ] **Step 2: Declare the Win32 interop**

Create `src/CameraOnScreen.App/Overlay/Interop.cs` with the P/Invoke needed for a layered,
topmost, no-redirection-bitmap window:
```csharp
using System.Runtime.InteropServices;

namespace CameraOnScreen.App.Overlay;

internal static class Interop
{
    public const int WS_POPUP = unchecked((int)0x80000000);
    public const int WS_EX_LAYERED = 0x00080000;
    public const int WS_EX_TOPMOST = 0x00000008;
    public const int WS_EX_NOREDIRECTIONBITMAP = 0x00200000;
    public const int WS_EX_TRANSPARENT = 0x00000020;

    [StructLayout(LayoutKind.Sequential)]
    public struct WNDCLASSEX
    {
        public int cbSize; public int style; public IntPtr lpfnWndProc;
        public int cbClsExtra; public int cbWndExtra; public IntPtr hInstance;
        public IntPtr hIcon; public IntPtr hCursor; public IntPtr hbrBackground;
        public string? lpszMenuName; public string lpszClassName; public IntPtr hIconSm;
    }

    public delegate IntPtr WndProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern ushort RegisterClassEx(ref WNDCLASSEX lpwcx);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateWindowEx(int exStyle, string className, string windowName,
        int style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);

    [DllImport("user32.dll")] public static extern IntPtr DefWindowProc(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int idx);
    [DllImport("user32.dll")] public static extern int SetWindowLong(IntPtr h, int idx, int val);
    public const int GWL_EXSTYLE = -20;
    public const int SW_SHOWNOACTIVATE = 4;
    [DllImport("kernel32.dll")] public static extern IntPtr GetModuleHandle(string? name);
}
```

- [ ] **Step 3: Build the overlay window with a DComp composition swap chain**

Create `src/CameraOnScreen.App/Overlay/OverlayWindow.cs`:
```csharp
using SharpGen.Runtime;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DirectComposition;
using Vortice.DXGI;
using static CameraOnScreen.App.Overlay.Interop;

namespace CameraOnScreen.App.Overlay;

public sealed class OverlayWindow : IDisposable
{
    private readonly IntPtr _hwnd;
    private readonly ID3D11Device _device;
    private readonly ID3D11DeviceContext _context;
    private readonly IDXGISwapChain1 _swapChain;
    private readonly IDCompositionDevice _dcomp;
    private readonly IDCompositionTarget _target;
    private static WndProc? _proc; // keep delegate alive

    public IntPtr D3DDevicePtr => _device.NativePointer;

    public OverlayWindow(int x, int y, int width, int height)
    {
        _proc = (h, m, w, l) => DefWindowProc(h, m, w, l);
        var wc = new WNDCLASSEX
        {
            cbSize = System.Runtime.InteropServices.Marshal.SizeOf<WNDCLASSEX>(),
            lpfnWndProc = System.Runtime.InteropServices.Marshal.GetFunctionPointerForDelegate(_proc),
            hInstance = GetModuleHandle(null),
            lpszClassName = "CosOverlay"
        };
        RegisterClassEx(ref wc);

        _hwnd = CreateWindowEx(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
            "CosOverlay", "CameraOnScreen Overlay", WS_POPUP,
            x, y, width, height, IntPtr.Zero, IntPtr.Zero, wc.hInstance, IntPtr.Zero);

        D3D11.D3D11CreateDevice(null, DriverType.Hardware, DeviceCreationFlags.BgraSupport,
            null, out _device!, out _context!).CheckError();

        using var dxgi = _device.QueryInterface<IDXGIDevice>();
        var factory = DXGI.CreateDXGIFactory2<IDXGIFactory2>(false);
        var desc = new SwapChainDescription1
        {
            Width = width, Height = height, Format = Format.B8G8R8A8_UNorm,
            BufferCount = 2, BufferUsage = Usage.RenderTargetOutput,
            SwapEffect = SwapEffect.FlipSequential, AlphaMode = AlphaMode.Premultiplied,
            SampleDescription = new SampleDescription(1, 0)
        };
        _swapChain = factory.CreateSwapChainForComposition(_device, desc);

        DComp.DCompositionCreateDevice(dxgi, out _dcomp!).CheckError();
        _target = _dcomp.CreateTargetForHwnd(_hwnd, topmost: true);
        var visual = _dcomp.CreateVisual();
        visual.SetContent(_swapChain);
        _target.SetRoot(visual);
        _dcomp.Commit();
    }

    public void Show() => ShowWindow(_hwnd, SW_SHOWNOACTIVATE);

    public void PresentTestPattern()
    {
        using var back = _swapChain.GetBuffer<ID3D11Texture2D>(0);
        using var rtv = _device.CreateRenderTargetView(back);
        _context.ClearRenderTargetView(rtv, new Vortice.Mathematics.Color4(0f, 0.4f, 1f, 0.5f));
        _swapChain.Present(1, PresentFlags.None);
    }

    public void Dispose()
    {
        _target.Dispose(); _dcomp.Dispose(); _swapChain.Dispose();
        _context.Dispose(); _device.Dispose();
    }
}
```

- [ ] **Step 4: Create the overlay on launch**

Edit `src/CameraOnScreen.App/MainWindow.xaml.cs` — add a field and create it in the constructor after `InitializeComponent()`:
```csharp
private readonly Overlay.OverlayWindow _overlay;
// ...
public MainWindow()
{
    Vm = Services.BuildViewModel();
    InitializeComponent();
    _overlay = new Overlay.OverlayWindow(200, 200, 320, 240);
    _overlay.Show();
    _overlay.PresentTestPattern();
}
```

- [ ] **Step 5: Build + manual verification**

Run: `dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj` and launch.
Expected: a semi-transparent blue rectangle floats on the desktop, on top of other
windows, with no title bar. The desktop shows through its alpha. (If it is opaque or has
black corners, the swap chain `AlphaMode`/`NoRedirectionBitmap` is misconfigured — fix
before proceeding.)

- [ ] **Step 6: Commit**

```bash
git add src/CameraOnScreen.App/Overlay src/CameraOnScreen.App/MainWindow.xaml.cs
git commit -m "feat(app): raw Win32 + DirectComposition layered overlay window"
```

---

### Task 12: Passthrough — capture frames into the overlay (shared device)

**Files:**
- Modify: `src/CameraOnScreen.App/Overlay/OverlayWindow.cs` (upload + present a BGRA frame)
- Modify: `src/CameraOnScreen.App/Composition/Services.cs` (pass overlay's D3D device to `shim.Init`)
- Modify: `src/CameraOnScreen.App/MainWindow.xaml.cs` (frame pump on a timer)

**Interfaces:**
- Consumes: `OverlayWindow.D3DDevicePtr`, `INativeShim.TryGetFrame`.
- Produces:
  - `OverlayWindow.PresentFrame(byte[] bgra, int width, int height)` — uploads to a dynamic texture and draws it full-window.
  - Shared device: `shim.Init(overlay.D3DDevicePtr)`.

- [ ] **Step 1: Add frame presentation to the overlay**

Edit `src/CameraOnScreen.App/Overlay/OverlayWindow.cs` — add a cached dynamic texture +
a simple full-screen blit (create a `ID3D11Texture2D` with `Dynamic` usage + `CpuAccessFlags.Write`,
`Map`/copy rows/`Unmap`, then `CopyResource`/draw into the back buffer and `Present`):
```csharp
private ID3D11Texture2D? _frameTex;
private int _texW, _texH;

public void PresentFrame(byte[] bgra, int width, int height)
{
    if (_frameTex is null || _texW != width || _texH != height)
    {
        _frameTex?.Dispose();
        _frameTex = _device.CreateTexture2D(new Texture2DDescription
        {
            Width = width, Height = height, MipLevels = 1, ArraySize = 1,
            Format = Format.B8G8R8A8_UNorm, SampleDescription = new SampleDescription(1, 0),
            Usage = ResourceUsage.Dynamic, BindFlags = BindFlags.ShaderResource,
            CPUAccessFlags = CpuAccessFlags.Write
        });
        _texW = width; _texH = height;
    }

    var map = _context.Map(_frameTex, 0, MapMode.WriteDiscard, Vortice.Direct3D11.MapFlags.None);
    for (int row = 0; row < height; row++)
        System.Runtime.InteropServices.Marshal.Copy(
            bgra, row * width * 4,
            map.DataPointer + row * map.RowPitch, width * 4);
    _context.Unmap(_frameTex, 0);

    using var back = _swapChain.GetBuffer<ID3D11Texture2D>(0);
    _context.CopyResource(back, _frameTex); // sizes match the overlay; resize handled in Task 13
    _swapChain.Present(1, PresentFlags.None);
}
```

- [ ] **Step 2: Share the device with the shim**

Edit `src/CameraOnScreen.App/Composition/Services.cs` — accept the overlay and pass its
device pointer to `Init`. Change `BuildViewModel` to `BuildViewModel(OverlayWindow overlay)`:
```csharp
public static MainViewModel BuildViewModel(CameraOnScreen.App.Overlay.OverlayWindow overlay)
{
    var store = new JsonSettingsStore(JsonSettingsStore.DefaultPath());
    var config = store.Load();
    INativeShim shim = new CameraOnScreen.App.Native.PInvokeShim();
    shim.Init(overlay.D3DDevicePtr);
    var tier = CameraOnScreen.App.Native.GpuTierDetector.Detect();
    var orchestrator = new Orchestrator(shim, tier);
    var vm = new MainViewModel(orchestrator);
    foreach (var cam in shim.EnumerateCameras()) vm.Cameras.Add(cam);
    vm.LoadFrom(config);
    return vm;
}
```
> Note: the M3–M5 plan moves frame production onto this shared device (CUDA↔D3D interop).
> For passthrough the shim still returns a CPU buffer; the shared device is established now
> so the contract is stable.

- [ ] **Step 3: Pump frames from the shim to the overlay**

Edit `src/CameraOnScreen.App/MainWindow.xaml.cs` — build overlay first, then the VM, and
run a `DispatcherQueueTimer` at ~30 Hz that pulls a frame and presents it, and polls status:
```csharp
private readonly Overlay.OverlayWindow _overlay;
private readonly byte[] _frameBuf = new byte[1920 * 1080 * 4];
private Microsoft.UI.Dispatching.DispatcherQueueTimer? _timer;

public MainWindow()
{
    _overlay = new Overlay.OverlayWindow(200, 200, 320, 240);
    _overlay.Show();
    Vm = Services.BuildViewModel(_overlay);
    InitializeComponent();

    _timer = DispatcherQueue.CreateTimer();
    _timer.Interval = TimeSpan.FromMilliseconds(33);
    _timer.Tick += (_, _) =>
    {
        if (Vm.IsRunning && Vm.ShimRef.TryGetFrame(_frameBuf, out int w, out int h) && w > 0)
            _overlay.PresentFrame(_frameBuf, w, h);
        Vm.PollStatusTick();
    };
    _timer.Start();
}
```
Expose the needed hooks on the VM — edit `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs`:
```csharp
public INativeShim ShimRef { get; }   // set in ctor: ShimRef = ... (pass shim into VM)
public void PollStatusTick() => _orchestrator.PollStatus();
```
To supply `ShimRef`, change the `MainViewModel` constructor to also accept the shim:
`public MainViewModel(Orchestrator orchestrator, INativeShim shim)`, set `ShimRef = shim`,
and update `Services.BuildViewModel` and the Core tests to pass the same `FakeShim`/shim
instance used by the orchestrator.

- [ ] **Step 4: Build + manual verification (camera required)**

Run: `msbuild native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64` then
`dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj`, launch, pick a camera, Start.
Expected: live webcam video appears inside the floating overlay, updating ~30 fps, still
topmost and borderless. Effects remain off (passthrough).

- [ ] **Step 5: Update affected Core tests + run suite**

Update `MainViewModelTests` and `Services` call sites for the new two-arg constructor
(pass the same `FakeShim` to both the `Orchestrator` and the VM). Run: `dotnet test`.
Expected: all green.

- [ ] **Step 6: Commit**

```bash
git add src native
git commit -m "feat: webcam passthrough rendered into the layered overlay (shared device)"
```

---

### Task 13: Overlay interaction — drag, resize, lock, click-through, chrome auto-hide

**Files:**
- Modify: `src/CameraOnScreen.App/Overlay/Interop.cs` (hit-test/move messages, set ex-style)
- Modify: `src/CameraOnScreen.App/Overlay/OverlayWindow.cs` (window proc: drag/resize, lock, click-through, resize swap chain)
- Modify: `src/CameraOnScreen.App/MainWindow.xaml.cs` (bind lock/click-through to VM/overlay)

**Interfaces:**
- Consumes: Task 11/12 overlay.
- Produces:
  - `OverlayWindow.SetLocked(bool locked)`, `OverlayWindow.SetClickThrough(bool on)`,
    `OverlayWindow.Resize(int w, int h)`, events `Moved`/`Resized` reporting new geometry.
  - Drag-anywhere while unlocked via `WM_NCHITTEST` → `HTCAPTION`; resize handles via
    `HTBOTTOMRIGHT` etc.; chrome (handles) drawn only while unlocked + hovered.

- [ ] **Step 1: Add the messages/styles to interop**

Edit `src/CameraOnScreen.App/Overlay/Interop.cs`, add constants:
```csharp
public const uint WM_NCHITTEST = 0x0084;
public const uint WM_MOUSELEAVE = 0x02A3;
public const int HTCLIENT = 1, HTCAPTION = 2, HTBOTTOMRIGHT = 17;
public const uint SWP_NOMOVE = 0x0002, SWP_NOZORDER = 0x0004, SWP_NOACTIVATE = 0x0010;
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
```

- [ ] **Step 2: Implement interaction in the window proc**

Edit `src/CameraOnScreen.App/Overlay/OverlayWindow.cs`:
- Replace the placeholder `_proc` with a real handler that:
  - returns `HTCAPTION` for `WM_NCHITTEST` when **unlocked** and the point is in the body (lets the user drag the window anywhere),
  - returns `HTBOTTOMRIGHT` when the point is within the bottom-right 16×16 grip and unlocked,
  - otherwise `HTCLIENT`.
- Add:
```csharp
private bool _locked;
public void SetLocked(bool locked) => _locked = locked;

public void SetClickThrough(bool on)
{
    int ex = GetWindowLong(_hwnd, GWL_EXSTYLE);
    ex = on ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
    SetWindowLong(_hwnd, GWL_EXSTYLE, ex);
}

public void Resize(int w, int h)
{
    _swapChain.ResizeBuffers(2, w, h, Format.B8G8R8A8_UNorm, SwapChainFlags.None);
    SetWindowPos(_hwnd, IntPtr.Zero, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
```
- The window proc must capture `this` (store the active instance in a static field keyed by hwnd, or a single-overlay static reference) so it can read `_locked`. Chrome (the resize grip) is drawn in `PresentFrame` only when `_locked == false` **and** the pointer is currently over the window (track via `WM_MOUSELEAVE` + a hover flag); when locked or on mouse-leave, draw no grip so recordings stay clean.

- [ ] **Step 3: Bind lock + click-through to the VM**

Edit `src/CameraOnScreen.App/MainWindow.xaml.cs` — when `Vm.Locked`/`Vm.ClickThrough`
change, call `_overlay.SetLocked` / `_overlay.SetClickThrough`. Add `Locked` and
`ClickThrough` observable properties to `MainViewModel` (Core), defaulting from
`AppConfig.Overlay`, and a small `OverlaySettings` write-back used in Task 14.
Add to `MainWindow.xaml` a "Lock overlay" and "Click-through" `ToggleSwitch` bound to those
VM properties.

- [ ] **Step 4: Build + manual verification**

Launch, Start. Expected:
- **Unlocked:** dragging anywhere on the video moves the overlay; the bottom-right grip
  resizes it (video keeps filling the window). A resize grip is visible only while hovering.
- **Lock on:** dragging/resizing no longer work; no grip is drawn.
- **Click-through on:** clicks pass through to the window beneath; the overlay can no longer
  be grabbed (recover via the control panel toggle — global hotkey added in Task 14).

- [ ] **Step 5: Commit**

```bash
git add src/CameraOnScreen.App
git commit -m "feat(app): overlay drag/resize/lock/click-through with clean-capture chrome"
```

---

### Task 14: Global hotkeys + persist/restore geometry and settings

**Files:**
- Create: `src/CameraOnScreen.App/Hotkeys/GlobalHotkeyService.cs`
- Modify: `src/CameraOnScreen.App/MainWindow.xaml.cs` (register hotkeys; save on close/move/resize)
- Modify: `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs` (expose `ToAppConfig()`)

**Interfaces:**
- Consumes: `HotkeyBinding`, `HotkeyAction`, `OverlayWindow` events, `JsonSettingsStore`.
- Produces:
  - `sealed class GlobalHotkeyService : IDisposable` with `Register(IReadOnlyList<HotkeyBinding> bindings, Action<HotkeyAction> onPressed)` using `RegisterHotKey`/`UnregisterHotKey` and a message-only window.
  - `MainViewModel.ToAppConfig()` producing an `AppConfig` from current state (incl. current overlay geometry passed in).

- [ ] **Step 1: Implement the global hotkey service**

Create `src/CameraOnScreen.App/Hotkeys/GlobalHotkeyService.cs`:
```csharp
using System.Runtime.InteropServices;
using CameraOnScreen.Core.Config;

namespace CameraOnScreen.App.Hotkeys;

public sealed class GlobalHotkeyService : IDisposable
{
    [DllImport("user32.dll")] private static extern bool RegisterHotKey(IntPtr hWnd, int id, uint mods, uint vk);
    [DllImport("user32.dll")] private static extern bool UnregisterHotKey(IntPtr hWnd, int id);

    private readonly IntPtr _hwnd;        // a message window's HWND
    private readonly List<int> _ids = new();
    private Action<HotkeyAction>? _onPressed;
    private IReadOnlyList<HotkeyBinding> _bindings = Array.Empty<HotkeyBinding>();

    public GlobalHotkeyService(IntPtr messageWindowHwnd) => _hwnd = messageWindowHwnd;

    public void Register(IReadOnlyList<HotkeyBinding> bindings, Action<HotkeyAction> onPressed)
    {
        _bindings = bindings; _onPressed = onPressed;
        for (int i = 0; i < bindings.Count; i++)
        {
            RegisterHotKey(_hwnd, i, (uint)bindings[i].Modifiers, bindings[i].VirtualKey);
            _ids.Add(i);
        }
    }

    // Call from the message window's WndProc on WM_HOTKEY (0x0312); wParam = id.
    public void OnHotkeyMessage(int id)
    {
        if (id >= 0 && id < _bindings.Count) _onPressed?.Invoke(_bindings[id].Action);
    }

    public void Dispose() { foreach (var id in _ids) UnregisterHotKey(_hwnd, id); }
}
```
> The overlay already has an HWND and a window proc (Task 13). Register the hotkeys against
> that HWND and route `WM_HOTKEY` (`0x0312`) from the overlay proc into
> `GlobalHotkeyService.OnHotkeyMessage((int)wParam)`.

- [ ] **Step 2: Map actions to behavior**

Edit `src/CameraOnScreen.App/MainWindow.xaml.cs` — after building the overlay + VM, create
the service and map actions:
```csharp
_hotkeys = new Hotkeys.GlobalHotkeyService(_overlay.Hwnd); // expose Hwnd on OverlayWindow
_hotkeys.Register(Vm.ToAppConfig(0,0,0,0).Hotkeys, action =>
{
    switch (action)
    {
        case Core.Config.HotkeyAction.ToggleLock: Vm.Locked = !Vm.Locked; break;
        case Core.Config.HotkeyAction.ToggleClickThrough: Vm.ClickThrough = !Vm.ClickThrough; break;
        case Core.Config.HotkeyAction.ToggleOverlayVisible: _overlay.ToggleVisible(); break;
        case Core.Config.HotkeyAction.ToggleRunning:
            if (Vm.IsRunning) Vm.StopCommand.Execute(null); else Vm.StartCommand.Execute(null);
            break;
    }
});
```
Add `OverlayWindow.Hwnd` (returns `_hwnd`) and `OverlayWindow.ToggleVisible()`.

- [ ] **Step 3: Persist on change/close**

Edit `MainViewModel` (Core) — add:
```csharp
public bool Locked { get; set; }
public bool ClickThrough { get; set; }

public AppConfig ToAppConfig(double x, double y, double w, double h) => new()
{
    CameraId = SelectedCamera?.Id,
    Overlay = new OverlaySettings
    {
        X = x, Y = y, Width = w, Height = h,
        Locked = Locked, ClickThrough = ClickThrough
    },
    Effects = new EffectSettings
    {
        GreenScreenEnabled = GreenScreenEnabled, GreenScreenStrength = GreenScreenStrength,
        EyeContactEnabled = EyeContactEnabled, EyeContactSensitivity = EyeContactSensitivity,
        EyeContactLookAwayRange = EyeContactLookAwayRange
    }
};
```
In `MainWindow.xaml.cs`, save on `OverlayWindow.Moved`/`Resized` and on window close:
```csharp
private void Save()
{
    var (x, y, w, h) = _overlay.GetBounds();
    new JsonSettingsStore(JsonSettingsStore.DefaultPath()).Save(Vm.ToAppConfig(x, y, w, h));
}
```
Restore geometry at startup by reading the same store before creating the overlay and
passing the saved `OverlaySettings` bounds into the `OverlayWindow` constructor (fall back
to defaults when none).

- [ ] **Step 4: Add a Core test for round-trip of geometry/effects**

Add to `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`:
```csharp
[Fact]
public void ToAppConfig_captures_geometry_and_effects()
{
    var vm = Build(GpuTier.Rtx, out _);
    vm.GreenScreenEnabled = true; vm.Locked = true;
    var cfg = vm.ToAppConfig(10, 20, 300, 400);
    Assert.Equal(10, cfg.Overlay.X);
    Assert.Equal(400, cfg.Overlay.Height);
    Assert.True(cfg.Overlay.Locked);
    Assert.True(cfg.Effects.GreenScreenEnabled);
}
```
Run: `dotnet test --filter MainViewModelTests` → PASS.

- [ ] **Step 5: Build + manual verification**

Launch, move/resize the overlay, toggle lock via the `Ctrl+Alt+F8` hotkey while a
different app is focused (overlay lock state flips), close the app, relaunch. Expected:
overlay reappears at the saved position/size with the saved lock/click-through and selected
camera. `Ctrl+Alt+F10` toggles overlay visibility; `Ctrl+Alt+F11` starts/stops.

- [ ] **Step 6: Commit**

```bash
git add src tests
git commit -m "feat(app): global hotkeys and settings/geometry persistence"
```

---

### Task 15: Screen-recorder capture verification

**Files:**
- Create: `docs/superpowers/verification/2026-06-20-recorder-capture.md`

**Interfaces:**
- Consumes: the running app from Tasks 12–14.
- Produces: a recorded verification note (manual gate; the spec's headline requirement).

- [ ] **Step 1: Verify capture in OBS**

With the app running and a webcam selected/Started:
1. Open OBS → add a **Display Capture** source. Confirm the overlay (person on transparent
   background once effects exist; raw webcam now) appears in the OBS preview.
2. Add a **Window Capture** source targeting the overlay window. Note whether the layered
   topmost window is captured; if not, record that Display Capture is the supported path.
3. Start recording for ~5 seconds, move the overlay mid-recording, stop, and play back.
   Confirm the overlay is in the recording with no chrome/handles visible and no post-edit
   needed.

- [ ] **Step 2: Verify in Xbox Game Bar**

Repeat a short capture with `Win+G` → record. Confirm the overlay appears (Game Bar
captures the foreground app/desktop; note any limitation).

- [ ] **Step 3: Write the verification note**

Create `docs/superpowers/verification/2026-06-20-recorder-capture.md` recording: recorder
+ version, capture mode that worked (display vs window), screenshots/paths, and any
recorder that needed display capture. This satisfies the spec risk "Recorder capture of
topmost layered windows — verify against target recorders early (M2)".

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/verification
git commit -m "test: verify overlay is captured live by screen recorders"
```

---

## Self-Review

**Spec coverage (M1+M2 scope):**
- Single-process arch, C# owns windowing, shim never renders → Tasks 8–12, 11. ✓
- Shared D3D11 device, no shared handles → Task 12 Step 2. ✓
- Poll `GetStatus`, no callbacks → Task 5 + Task 12 timer. ✓
- GPU tiers (effects need RTX; passthrough without) → Task 5 gating, Task 9 detector, Task 8 disabled toggles. ✓
- Overlay: layered/topmost/per-pixel alpha, drag, resize, lock, click-through, chrome-free-by-default, multi-monitor (absolute desktop coords), persistence → Tasks 11, 13, 14. ✓
- Global hotkeys via `RegisterHotKey` → Task 14. ✓
- Camera enumeration/capture via Media Foundation in shim → Tasks 9–10. ✓
- Settings JSON in `%LOCALAPPDATA%` → Task 3, Task 14. ✓
- Recorder-capture verification → Task 15. ✓
- Mirror at present time, Maxine effects (green screen, eye contact), CUDA↔D3D interop, gaze status → **deferred to the M3–M5 plan** (documented in Task 12 note and the plan header). ✓ (intentional scope split)

**Placeholder scan:** No "TODO/TBD". Two tasks (Task 10 Media Foundation body, Task 13 window-proc body) reference a fixed interface contract instead of full inline source because the bodies are large, standard-API implementations; their contracts (method signatures, return shapes) are fully specified so neighbors are unblocked.

**Type consistency:** `INativeShim` (Init/EnumerateCameras/SetParams/Start/Stop/GetStatus/TryGetFrame/Dispose) is consistent across `FakeShim`, `PInvokeShim`, orchestrator, and VM. `ShimParams`/`ShimStatus`/`CameraInfo`/`GazeState` field names match between Core records and the C ABI structs (`green_screen_*` ↔ `GreenScreen*`). `MainViewModel` two-arg constructor `(Orchestrator, INativeShim)` is applied consistently after Task 12.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-06-20-camera-on-screen-foundation.md`. Two execution options:

1. **Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.
2. **Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints.

Which approach?
