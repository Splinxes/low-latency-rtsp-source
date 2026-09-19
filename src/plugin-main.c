// SPDX-License-Identifier: GPL-2.0-or-later

#include <obs-module.h>
#include <gst/gst.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("low-latency-rtsp", "en-US")

extern struct obs_source_info low_latency_rtsp_source_info;

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Low-latency RTSP source with automatic H.264/H.265 video detection and optional audio";
}

#ifdef _WIN32
static bool configure_gstreamer_runtime(void)
{
    wchar_t root[32768] = {0};
    wchar_t program_files[32768] = {0};
    wchar_t bin_dir[32768] = {0};
    wchar_t plugin_dir[32768] = {0};

    DWORD root_len = GetEnvironmentVariableW(
        L"GSTREAMER_1_0_ROOT_MSVC_X86_64", root,
        (DWORD)(sizeof(root) / sizeof(root[0])));

    if (root_len == 0 || root_len >= (DWORD)(sizeof(root) / sizeof(root[0]))) {
        DWORD pf_len = GetEnvironmentVariableW(
            L"ProgramFiles", program_files,
            (DWORD)(sizeof(program_files) / sizeof(program_files[0])));
        if (pf_len == 0 || pf_len >= (DWORD)(sizeof(program_files) / sizeof(program_files[0]))) {
            blog(LOG_ERROR, "[low-latency-rtsp] Could not locate Program Files for GStreamer runtime");
            return false;
        }

        if (swprintf_s(root, sizeof(root) / sizeof(root[0]),
                       L"%ls\\gstreamer\\1.0\\msvc_x86_64", program_files) < 0)
            return false;
    }

    if (swprintf_s(bin_dir, sizeof(bin_dir) / sizeof(bin_dir[0]),
                   L"%ls\\bin", root) < 0 ||
        swprintf_s(plugin_dir, sizeof(plugin_dir) / sizeof(plugin_dir[0]),
                   L"%ls\\lib\\gstreamer-1.0", root) < 0)
        return false;

    if (GetFileAttributesW(bin_dir) == INVALID_FILE_ATTRIBUTES) {
        blog(LOG_ERROR, "[low-latency-rtsp] GStreamer runtime bin directory was not found");
        return false;
    }

    if (!SetDllDirectoryW(bin_dir)) {
        blog(LOG_ERROR,
             "[low-latency-rtsp] Failed to configure GStreamer DLL search path (Win32=%lu)",
             (unsigned long)GetLastError());
        return false;
    }

    wchar_t existing_plugin_path[2] = {0};
    DWORD existing_len = GetEnvironmentVariableW(
        L"GST_PLUGIN_PATH_1_0", existing_plugin_path,
        (DWORD)(sizeof(existing_plugin_path) / sizeof(existing_plugin_path[0])));
    if (existing_len == 0)
        SetEnvironmentVariableW(L"GST_PLUGIN_PATH_1_0", plugin_dir);

    blog(LOG_INFO, "[low-latency-rtsp] GStreamer runtime search path configured");
    return true;
}
#endif

bool obs_module_load(void)
{
#ifdef _WIN32
    if (!configure_gstreamer_runtime())
        return false;
#endif

    GError *error = NULL;
    if (!gst_init_check(NULL, NULL, &error)) {
        blog(LOG_ERROR, "[low-latency-rtsp] GStreamer initialization failed");
        if (error)
            g_error_free(error);
        return false;
    }

    obs_register_source(&low_latency_rtsp_source_info);

    guint major = 0, minor = 0, micro = 0, nano = 0;
    gst_version(&major, &minor, &micro, &nano);
    blog(LOG_INFO,
         "[low-latency-rtsp] Loaded v0.4.0 with GStreamer %u.%u.%u",
         major, minor, micro);
    return true;
}
