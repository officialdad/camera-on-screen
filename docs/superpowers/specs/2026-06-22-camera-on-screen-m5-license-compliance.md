# M5 — Maxine license / redistribution compliance

Resolves GitHub issue **#3** ("Full Maxine license/EULA compliance review before
public release"). This is the standing compliance record for redistributing the
bundled NVIDIA Maxine runtime in the installer (`scripts/build-installer.ps1` +
`scripts/bundle-maxine.ps1`).

## Governing license

The redistribution-specific **NVIDIA Maxine SDK License Agreement (v. April 1,
2021) + Supplement**, served at
<https://developer.nvidia.com/downloads/maxine-sdk-license> (redirects to
`developer.download.nvidia.com/licenses/Maxine_SDK_License_1Apr2021_updated.pdf`).

> **Document discrepancy (noted, not blocking).** The SDK *runtime* directory
> bundles a *different, generic* `NVIDIA Maxine EULA.pdf` — the NVIDIA Software
> License Agreement (June 2024) + AI Product-Specific Terms (June 2024). Both
> documents grant redistribution of the runtime inside an application; their
> conditions are compatible. We comply with the **superset** of both.

### Grant — redistribution is permitted

- **Agreement §1.1(iii) + Supplement §2.** Distributable = "any portion of the
  SDK **other than the audio/video data samples**," incorporated in object-code
  form into an application that meets the distribution requirements. The runtime
  DLLs and the `*.engine.trtpkg` model files are therefore distributable, at no
  charge.
- **Supplement §1.** Licensed for applications "only for use in systems with
  NVIDIA GPUs." Satisfied: the AI effects are RTX-gated; on non-NVIDIA systems
  the app runs as a plain overlay with effects disabled.
- NVIDIA's VFX SDK System Guide independently documents this model: "the
  developer can package the runtime dependencies into the application or require
  application users to use the SDK installer."

**Precedent:** *VTube Studio – NVIDIA Broadcast Tracker* (Steam) ships the NVIDIA
AR SDK in a commercial third-party desktop application.

### Conditions (Agreement §1.2 + §2 + Supplement §3) and how we meet them

| Clause | Requirement | How satisfied |
| --- | --- | --- |
| §1.2(i) | Material additional functionality beyond the SDK | Webcam desktop-overlay app; the SDK is one feature |
| §1.2(ii) | Distributable portions accessed only by your application | Bundled under `<app>\maxine\`, loaded only by the shim |
| §1.2(iii) | Include "This software contains source code provided by NVIDIA Corporation." in distributed derivatives of sample source | `THIRD-PARTY-NOTICES.md` → "NVIDIA source-code notice" |
| §1.2(v) | End-user terms consistent with NVIDIA's license | `installer/COMBINED-LICENSE.txt` shown as the install license page (not bare MIT) |
| §2.1 | Do not remove NVIDIA copyright/proprietary notices | Notices copied verbatim; DLLs unmodified |
| §2.5 | Do not subject the SDK to an OSS license | `THIRD-PARTY-NOTICES.md` states MIT covers our code only, not the NVIDIA components |
| Suppl. §3.1 | Attribute SDK use per NVIDIA branding guidelines | Good-faith attribution wired: `AppInfo.MaxineAttribution` → control-panel footer. Exact portal string still to confirm (see below) |

## Bundled-file compliance matrix

| Class | Files | License | Notice shipped |
| --- | --- | --- | --- |
| Compiled shim (our code) | `CameraOnScreen.Shim.dll` (incl. NVIDIA proxy-stub source) | MIT (our code) + NVIDIA header grant; §1.2(iii) notice | `LICENSE`, `THIRD-PARTY-NOTICES.md` |
| Maxine effect DLLs | `NVVideoEffects.dll`, `nvARPose.dll`, `NVCVImage.dll` | NVIDIA Maxine SDK License | `maxine\NVIDIA Maxine EULA.pdf` |
| CUDA / TensorRT / NPP / misc runtime | `nvinfer_10`, `cudart64_12`, `cublasLt64_12`, `npp*`, `cufft64_11`, `nppif64_12`, `nvonnxparser_10`, `libcrypto-3-x64` | NVIDIA EULA + bundled OSS notices | `maxine\ThirdPartyLicenses-VFX.txt` + `maxine\ThirdPartyLicenses-AR.txt` |
| Models | `AIGS_*_86`, `gazeredir_*_86`, `face_detection_86`, `faceland_*_86` | NVIDIA Maxine SDK License ("Model") | `maxine\NVIDIA Maxine EULA.pdf` |

> The two SDKs ship a same-named `ThirdPartyLicenses.txt` with **different**
> content (VFX ~1.4 MB, AR ~466 KB). The bundler carries **both** under distinct
> names (`-VFX` / `-AR`) — previously only the VFX copy travelled, dropping the
> AR-only OSS notices (`cufft64_11`, `nppif64_12`, `nvARPose` deps).

## Changes made for #3

- `native/shim/bundle/maxine-manifest.psd1` — `License` array replaced with
  `VfxLicense` / `ArLicense` source→dest maps.
- `scripts/bundle-maxine.ps1` — license files are a **required** copy (throws if
  missing, no longer best-effort) and both SDKs' notices are carried under
  distinct names. (`scripts/Bundle-Maxine.Tests.ps1` covers both.)
- `installer/COMBINED-LICENSE.txt` — new combined end-user license (MIT app +
  NVIDIA Maxine flow-down).
- `installer/CameraOnScreen.iss` — `LicenseFile=COMBINED-LICENSE.txt`; ships
  `THIRD-PARTY-NOTICES.md` into the install dir. (`scripts/Build-Installer.Tests.ps1` covers both.)
- `THIRD-PARTY-NOTICES.md` — rewritten for binary distribution (MIT-scope
  clarification, §1.2(iii) notice, per-SDK notice table, attribution).
- `src/CameraOnScreen.Core/AppInfo.cs` — `MaxineAttribution` (Supplement §3.1) +
  `MaxineTrademarkNotice`, surfaced in the control-panel footer
  (`MainWindow.xaml` + code-behind). `tests/.../AppInfoTests.cs` enforces the format.

## Verification rule

A shipped bundle is compliant only if **every bundled-file class above maps to a
license text that is also shipped**, and the combined end-user license is the
install license page. The bundler enforces the notice files as required copies;
`Build-Installer.Tests.ps1` enforces the license page + notices shipment.

## Open — residual human check (does not block the build)

**Supplement §3.1 exact attribution wording.** NVIDIA's Maxine branding guidelines
live behind an interactive, login-gated brand portal
(`nvidia.com/maxine-sdk-guidelines` → `brand.nvidia.com`) that cannot be read
programmatically (confirmed inaccessible). A **good-faith attribution is wired**
("AI effects powered by NVIDIA® Maxine™", control-panel footer) using NVIDIA's
documented trademark-attribution format. If the portal (or
`maxinesdk-support@nvidia.com`) prescribes different exact wording/placement,
update the single constant `AppInfo.MaxineAttribution`.

The legal **redistribution grant is cleared** (Maxine SDK License 1 Apr 2021,
forum-corroborated as the governing doc). Recommend confirming the §3.1 string
with NVIDIA support before the public release (#4), but the bundle ships
compliant in good faith today.
