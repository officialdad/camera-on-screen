#pragma once
#include <cstdint>
#include <string>

// Hardware-agnostic green-screen engine (issue #24): the MediaPipe selfie-segmentation
// model (256x256, Apache-2.0) run in ONNX Runtime on the CPU. Same contract as Aigs:
// processes a tightly-packed BGRA buffer in place — A = matte, RGB premultiplied.
// ONNX Runtime is dlopen'd at runtime from COS_SEG_RUNTIME_DIR or <shim>/onnx/
// (libonnxruntime.so.1 / onnxruntime.dll + selfie_segmentation.onnx). Absent runtime
// means Probe()/Start() fail with a detail string — no hard dependency, the shim
// still loads everywhere. All methods are no-throw; failure via IsReady()+LastError().
class SegOnnx {
public:
    SegOnnx();
    ~SegOnnx();

    // One-shot probe: can ORT load and a session be created on the model? Does not
    // retain the session. Not designed for concurrent calls. Fills 'detail'.
    static bool Probe(std::string& detail);

    // Load ORT + create the inference session. Call on the capture worker thread.
    bool Start();

    // Destroy the session. Call on the worker thread. The ORT library itself stays
    // loaded process-wide (one runtime per process, like the Maxine preload).
    void Stop();

    // Run segmentation on a tightly-packed BGRA buffer (width*height*4) in place:
    // A = matte, RGB premultiplied. expand/feather (0..1) dilate then blur the matte.
    // Returns true if applied; false leaves 'bgra' untouched.
    bool ProcessFrame(uint8_t* bgra, int width, int height, double expand, double feather);

    bool IsReady() const { return ready_; }
    const std::string& LastError() const { return lastError_; }

private:
    bool ready_ = false;
    std::string lastError_;
    void* impl_ = nullptr; // opaque; real fields live in seg_onnx.cpp
};
