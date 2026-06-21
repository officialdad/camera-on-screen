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
    double green_screen_expand;       // 0..1 matte dilate (was green_screen_strength, unused)
    double green_screen_feather;      // 0..1 matte blur
    int    eye_contact_enabled;
    double eye_contact_sensitivity;
    double eye_contact_look_away_range;
} CosParams;

typedef struct {
    int  green_screen_available; // 1 if Maxine GreenScreen can run, else 0
    char detail[256];            // human-readable status/error (UTF-8, NUL-terminated)
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
// Probes whether AI Green Screen is available (SDK loads + effect creates + model loads).
// Fills *out. Returns 1 if available, 0 otherwise. Safe to call before cos_start.
COS_API int  cos_query_capabilities(CosCaps* out);
