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
// Copies latest BGRA8 frame into dst (width*height*4 bytes). Returns 1 if a new frame was copied.
COS_API int  cos_get_frame(uint8_t* dst, int* width, int* height, int dst_capacity);
COS_API void cos_shutdown(void);
