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
static bool trim_last_path_component(wchar_t *path)
{
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash)
        return false;

    *slash = L'\0';
    return true;
}

static bool find_bundled_gstreamer_root(wchar_t *root, size_t root_count)
{
    HMODULE module = NULL;
    wchar_t module_path[32768] = {0};

    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)(const void *)&find_bundled_gstreamer_root,
            &module))
        return false;

    DWORD length = GetModuleFileNameW(
        module, module_path,
        (DWORD)(sizeof(module_path) / sizeof(module_path[0])));
    if (length == 0 ||
        length >= (DWORD)(sizeof(module_path) / sizeof(module_path[0])))
        return false;

    /* DLL is <plugin root>\bin\64bit\low-latency-rtsp.dll. */
    if (!trim_last_path_component(module_path) ||
        !trim_last_path_component(module_path) ||
        !trim_last_path_component(module_path))
        return false;

    if (swprintf_s(root, root_count, L"%ls\\runtime", module_path) < 0)
        return false;

    wchar_t core_dll[32768] = {0};
    if (swprintf_s(core_dll,
                   sizeof(core_dll) / sizeof(core_dll[0]),
                   L"%ls\\bin\\gstreamer-1.0-0.dll",
                   root) < 0)
        return false;

    return GetFileAttributesW(core_dll) != INVALID_FILE_ATTRIBUTES;
}

static bool configure_gstreamer_runtime(void)
{
    wchar_t root[32768] = {0};
    wchar_t program_files[32768] = {0};
    wchar_t bin_dir[32768] = {0};
    wchar_t plugin_dir[32768] = {0};
    wchar_t scanner[32768] = {0};
    bool bundled = find_bundled_gstreamer_root(
        root, sizeof(root) / sizeof(root[0]));

    if (!bundled) {
        DWORD root_len = GetEnvironmentVariableW(
            L"GSTREAMER_1_0_ROOT_MSVC_X86_64", root,
            (DWORD)(sizeof(root) / sizeof(root[0])));

        if (root_len == 0 ||
            root_len >= (DWORD)(sizeof(root) / sizeof(root[0]))) {
            DWORD pf_len = GetEnvironmentVariableW(
                L"ProgramFiles", program_files,
                (DWORD)(sizeof(program_files) / sizeof(program_files[0])));
            if (pf_len == 0 ||
                pf_len >=
                    (DWORD)(sizeof(program_files) /
                            sizeof(program_files[0]))) {
                blog(LOG_ERROR,
                     "[low-latency-rtsp] Could not locate Program Files for GStreamer runtime");
                return false;
            }

            if (swprintf_s(root, sizeof(root) / sizeof(root[0]),
                           L"%ls\\gstreamer\\1.0\\msvc_x86_64",
                           program_files) < 0)
                return false;
        }
    }

    if (swprintf_s(bin_dir, sizeof(bin_dir) / sizeof(bin_dir[0]),
                   L"%ls\\bin", root) < 0 ||
        swprintf_s(plugin_dir,
                   sizeof(plugin_dir) / sizeof(plugin_dir[0]),
                   L"%ls\\lib\\gstreamer-1.0", root) < 0 ||
        swprintf_s(scanner, sizeof(scanner) / sizeof(scanner[0]),
                   L"%ls\\libexec\\gstreamer-1.0\\gst-plugin-scanner.exe",
                   root) < 0)
        return false;

    if (GetFileAttributesW(bin_dir) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(plugin_dir) == INVALID_FILE_ATTRIBUTES) {
        blog(LOG_ERROR,
             "[low-latency-rtsp] GStreamer runtime directories were not found");
        return false;
    }

    if (!SetDllDirectoryW(bin_dir)) {
        blog(LOG_ERROR,
             "[low-latency-rtsp] Failed to configure GStreamer DLL search path (Win32=%lu)",
             (unsigned long)GetLastError());
        return false;
    }

    if (bundled) {
        SetEnvironmentVariableW(L"GST_PLUGIN_PATH_1_0", plugin_dir);
        SetEnvironmentVariableW(L"GST_PLUGIN_SYSTEM_PATH_1_0", plugin_dir);

        if (GetFileAttributesW(scanner) != INVALID_FILE_ATTRIBUTES)
            SetEnvironmentVariableW(L"GST_PLUGIN_SCANNER_1_0", scanner);

        /*
         * gst-plugin-scanner.exe is a child process. SetDllDirectoryW() only
         * affects this OBS process, so prepend the private runtime bin folder
         * to PATH as well so the scanner can resolve its GStreamer/GLib DLLs.
         */
        wchar_t current_path[32768] = {0};
        wchar_t private_path[32768] = {0};
        DWORD path_len = GetEnvironmentVariableW(
            L"PATH", current_path,
            (DWORD)(sizeof(current_path) / sizeof(current_path[0])));

        if (path_len == 0) {
            SetEnvironmentVariableW(L"PATH", bin_dir);
        } else if (path_len <
                   (DWORD)(sizeof(current_path) / sizeof(current_path[0])) &&
                   swprintf_s(private_path,
                              sizeof(private_path) / sizeof(private_path[0]),
                              L"%ls;%ls", bin_dir, current_path) >= 0) {
            SetEnvironmentVariableW(L"PATH", private_path);
        }

        blog(LOG_INFO,
             "[low-latency-rtsp] Using bundled private GStreamer runtime");
    } else {
        wchar_t existing_plugin_path[2] = {0};
        DWORD existing_len = GetEnvironmentVariableW(
            L"GST_PLUGIN_PATH_1_0", existing_plugin_path,
            (DWORD)(sizeof(existing_plugin_path) /
                    sizeof(existing_plugin_path[0])));
        if (existing_len == 0)
            SetEnvironmentVariableW(L"GST_PLUGIN_PATH_1_0", plugin_dir);

        blog(LOG_INFO,
             "[low-latency-rtsp] Using external GStreamer runtime");
    }

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
         "[low-latency-rtsp] Loaded v0.5.1 with GStreamer %u.%u.%u",
         major, minor, micro);
    return true;
}
