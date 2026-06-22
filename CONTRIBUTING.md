# Contributing

Thanks for your interest in Camera-on-Screen. Issues and pull requests are
welcome.

## Before you start

- **Bugs / features** — open a [GitHub issue](https://github.com/officialdad/camera-on-screen/issues)
  first so we can agree on scope before code is written.
- Check existing issues; deferred work is tracked there.

## Environment

This is a **Windows + NVIDIA RTX** project. To build and run the full app with
effects you need:

- Windows 10/11 with an **NVIDIA RTX GPU** (Ampere / RTX 30-series) + recent driver
- .NET 8 SDK
- VS2022 Build Tools + MSVC v143
- The NVIDIA Maxine **Video Effects** and **AR** SDKs (download from
  <https://developer.nvidia.com/maxine>; not bundled — see
  [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md))

Without the Maxine SDKs the shim still builds as a CI-safe **passthrough stub**,
so you can work on most of the app on non-RTX machines — the AI effects are just
disabled.

Build steps and the non-obvious toolchain notes live in
[`CLAUDE.md`](CLAUDE.md) and the [README](README.md#build).

## Pull requests

- Keep PRs focused — one logical change per PR.
- **Builds and tests must be pristine (0 warnings).** CI enforces
  warnings-as-errors; a warning fails the build.
- Run the Core unit tests before pushing:
  `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
- Match the surrounding code style and comment density.
- **Read the relevant spec** in `docs/superpowers/specs/` before changing
  contracts that span components (the shim C ABI, the capture/effects pipeline,
  persistence).
- CI runs on a self-hosted RTX runner; PRs from outside contributors need a
  maintainer to approve the workflow run.

## License

By contributing you agree your contributions are licensed under the
[MIT License](LICENSE).
