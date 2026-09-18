#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Provided by libarmsx.dylib (__DLL_BUILD)
int external_main(int argc, const char* argv[], void* external_window, void* external_renderer);
void psxe_enqueue_launch_argument(const char* argument);
typedef void (*psxe_library_directory_picker_callback_t)(int recursive, void* userdata);
typedef void (*psxe_library_directory_removed_callback_t)(const char* path, void* userdata);
typedef void (*psxe_gravity_motion_enabled_callback_t)(int enabled, void* userdata);
void psxe_set_library_directory_picker_callback(
    psxe_library_directory_picker_callback_t callback,
    void* userdata
);
void psxe_set_library_directory_removed_callback(
    psxe_library_directory_removed_callback_t callback,
    void* userdata
);
void psxe_register_platform_library_directory(const char* path, const char* label, int recursive);
void psxe_set_gravity_motion_enabled_callback(
    psxe_gravity_motion_enabled_callback_t callback,
    void* userdata
);
void psxe_set_gravity_sample(float screen_x);

#ifdef __cplusplus
}
#endif
