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

/*
 * Show or hide the privacy-safe inline UniFi Protect URL guidance in an open
 * Properties dialog. The pasted URL itself is never shown.
 */
void llrtsp_ui_update_unifi_hint(const char *source_name, bool visible);

#ifdef __cplusplus
}
#endif
