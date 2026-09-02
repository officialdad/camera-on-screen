#pragma once
#include <cstddef>
#include <string>

// Free-VRAM gate for the Maxine / FRUC engine loads.
//
// NVIDIA's engine loaders (TensorRT deserialize inside libnvVFXGreenScreen /
// libnvARGazeRedirection) do not check their own GPU allocations: when the card is nearly
// full they dereference a null/garbage pointer and SIGSEGV the whole process instead of
// returning an error (cost a debugging cycle, 2026-09-02: a vLLM + sd-server tenant left
// ~300 MiB free and every launch died inside NvAR_Load). The only defence is to not call
// them — so every load site asks here first and reports a user-facing reason instead.
//
// The driver API is loaded at runtime (nvcuda.dll / libcuda.so.1) so the shim keeps ZERO
// NVIDIA link-time dependencies and still loads on non-NVIDIA boxes. Thread-safe.
namespace gpumem {

// Free VRAM the effects need before a load is attempted. Measured on RTX 3090 / sm86 engines:
// the capability probe (GreenScreen + GazeRedirection + FRUC, sequential) peaks ~1.3 GiB
// above baseline; a load with ~1.2 GiB free still crashed. COS_GPU_MIN_FREE_MIB overrides.
constexpr size_t kDefaultMinFreeMiB = 1536;

struct Info { size_t freeMiB = 0; size_t totalMiB = 0; };

// Free/total VRAM of CUDA device 0. False (with `err`) when there is no NVIDIA driver, no
// device, or the primary CUDA context cannot be created.
bool Query(Info& out, std::string& err);

// Effective floor in MiB (env override, else kDefaultMinFreeMiB).
size_t MinFreeMiB();

// True when an engine load may proceed: enough free VRAM, or no NVIDIA driver/device to ask
// (the SDK then reports its own failure as before). False with a short user-facing `reason`
// when free VRAM is under the floor or the CUDA context itself failed to come up — callers
// must NOT touch NvVFX_Load / NvAR_Load / NvOFFRUCCreate in that case.
bool CanLoad(std::string& reason);

}
