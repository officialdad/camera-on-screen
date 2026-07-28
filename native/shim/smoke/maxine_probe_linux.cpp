// Spike A + C probe (issue #27): load Maxine VFX green screen AND AR gaze redirection
// in ONE process on Linux, mirroring the shim's Probe() sequences. Verifies:
//   Spike A — in-process (non-Triton) gaze loads and runs on Linux + RTX
//   Spike C — VFX 1.2.0.0 + AR 1.1.1.0 Linux runtimes coexist (one TRT/CUDA stack)
// Build/run: see command line — links libVideoFX.so + libnvARPose.so directly.
#include <cstdio>
#include <string>
#include "nvVideoEffects.h"
#include "nvVFXGreenScreen.h"
#include "nvAR.h"
#include "nvAR_defs.h"

#ifndef NvAR_Feature_GazeRedirection
#define NvAR_Feature_GazeRedirection "GazeRedirection"
#endif

static bool ProbeGreenScreen(const char* modelDir) {
    NvVFX_Handle eff = nullptr;
    NvCV_Status s = NvVFX_CreateEffect(NVVFX_FX_GREEN_SCREEN, &eff);
    if (s != NVCV_SUCCESS || !eff) { std::printf("AIGS: CreateEffect FAILED (%d)\n", s); return false; }
    NvVFX_SetString(eff, NVVFX_MODEL_DIRECTORY, modelDir);
    NvVFX_SetU32(eff, NVVFX_MODE, 0u);
    NvVFX_SetU32(eff, NVVFX_MAX_INPUT_WIDTH, 1920u);
    NvVFX_SetU32(eff, NVVFX_MAX_INPUT_HEIGHT, 1080u);
    NvCV_Status load = NvVFX_Load(eff);
    NvVFX_DestroyEffect(eff);
    if (load != NVCV_SUCCESS) { std::printf("AIGS: NvVFX_Load FAILED (%d)\n", load); return false; }
    std::printf("AIGS: GreenScreen loaded OK\n");
    return true;
}

static bool ProbeGaze(const char* modelDir) {
    CUstream stream = nullptr;
    if (NvAR_CudaStreamCreate(&stream) != NVCV_SUCCESS) { std::printf("GAZE: CudaStreamCreate FAILED\n"); return false; }
    NvAR_FeatureHandle h = nullptr;
    NvCV_Status s = NvAR_Create(NvAR_Feature_GazeRedirection, &h);
    if (s != NVCV_SUCCESS || !h) { NvAR_CudaStreamDestroy(stream); std::printf("GAZE: NvAR_Create FAILED (%d)\n", s); return false; }
    NvAR_SetString(h, NvAR_Parameter_Config(ModelDir), modelDir);
    NvAR_SetU32(h, NvAR_Parameter_Config(Landmarks_Size), 126u);
    NvAR_SetU32(h, NvAR_Parameter_Config(Temporal), 0xFFFFFFFFu);
    NvAR_SetU32(h, NvAR_Parameter_Config(GazeRedirect), 1u);
    NvAR_SetCudaStream(h, NvAR_Parameter_Config(CUDAStream), stream);
    NvAR_SetU32(h, NvAR_Parameter_Config(EyeSizeSensitivity), 3u);
    NvCV_Status load = NvAR_Load(h);
    NvAR_Destroy(h);
    NvAR_CudaStreamDestroy(stream);
    if (load != NVCV_SUCCESS) { std::printf("GAZE: NvAR_Load FAILED (%d)\n", load); return false; }
    std::printf("GAZE: GazeRedirection loaded OK\n");
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: %s <vfx_model_dir> <ar_model_dir>\n", argv[0]); return 2; }
    bool a = ProbeGreenScreen(argv[1]);
    bool b = ProbeGaze(argv[2]);
    std::printf("RESULT: greenscreen=%s gaze=%s coexist=%s\n",
        a ? "PASS" : "FAIL", b ? "PASS" : "FAIL", (a && b) ? "PASS" : "FAIL");
    return (a && b) ? 0 : 1;
}
