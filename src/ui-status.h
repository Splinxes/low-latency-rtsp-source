// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Queue a status-only refresh for an open Low Latency RTSP Properties dialog.
 * This intentionally bypasses obs_source_update_properties(), which rebuilds
 * the entire property tree and disrupts password visibility/dropdown state.
 */
void llrtsp_ui_update_status(const char *source_name, const char *status_text,
                             int info_type);

/* Copy privacy-safe diagnostics to the system clipboard on OBS's UI thread. */
void llrtsp_ui_copy_text(const char *text);

/* Open a trusted project URL in the user's default browser. */
void llrtsp_ui_open_url(const char *url);

/*
 * Append or remove privacy-safe UniFi Protect URL guidance from the existing
 * top information label in an open Properties dialog. The pasted URL itself
 * is never shown.
 */
void llrtsp_ui_update_unifi_hint(const char *source_name,
                                 const char *base_info_text,
                                 const char *hint_text, bool visible);

#ifdef __cplusplus
}
#endif
