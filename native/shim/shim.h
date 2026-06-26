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
    int   exposure_supported; // 1 if the open camera exposes manual exposure (known only while running)
    char  error[256];        // empty string = no error
} CosStatus;

typedef struct {
    const char* camera_id;   // UTF-8, may be null
    int    green_screen_enabled;
    double green_screen_expand;       // 0..1 matte dilate (was green_screen_strength, unused)
    double green_screen_feather;      // 0..1 matte blur
    int    eye_contact_enabled;
    double eye_contact_sensitivity;
    double eye_contact_look_away_range;
    int    super_res_enabled;
    int    super_res_scale;           // 0=off, 15=1.5x, 20=2x (upscale modes only)
    int    super_res_quality_level;   // VSR QualityLevel: 1-4 upscale, 8-11 denoise, 12-15 deblur
    int    exposure_lock_enabled;     // 1 = manual exposure (locks fps); 0 = auto (default)
    double exposure_value;            // 0..1 normalized; native maps to camera's IAMCameraControl range
    int    frame_interp_enabled;
} CosParams;

typedef struct {
    int  green_screen_available; // 1 if Maxine GreenScreen can run, else 0
    char detail[256];            // green-screen status/error (UTF-8, NUL-terminated)
    int  eye_contact_available;  // 1 if Maxine GazeRedirection can run, else 0
    char ec_detail[256];         // eye-contact status/error (UTF-8, NUL-terminated)
    int  super_res_available;          // 1 if Maxine SuperRes can run
    int  frame_interp_available;
    char fi_detail[256];
} CosCaps;

COS_API int  cos_init(void* d3d11_device);
// Writes up to max ids/names (UTF-8, '\0'-terminated, 128 bytes each) into the buffers; returns count.
COS_API int  cos_enumerate_cameras(char* ids, char* names, int max);
COS_API void cos_set_params(const CosParams* p);
COS_API void cos_start(void);
COS_API void cos_stop(void);
COS_API void cos_get_status(CosStatus* out);
// Copies latest BGRA8 frame into dst (width*height*4 bytes). Returns 1 if a new frame was copied.
COS_API int  cos_get_frame(uint8_t* dst, int* width, int* height, int dst_capacity);
COS_API void cos_shutdown(void);
// Probes AI Green Screen and Eye Contact availability (each SDK loads + effect creates +
// model loads). Fills both gates in *out. Returns 1 if EITHER effect is available, 0 if
// neither; callers read the per-gate ints. Safe to call before cos_start.
COS_API int  cos_query_capabilities(CosCaps* out);
