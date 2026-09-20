// SPDX-License-Identifier: GPL-2.0-or-later

#include "ui-status.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QLabel>
#include <QMetaObject>
#include <QMessageBox>
#include <QPushButton>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStyle>
#include <QString>
#include <QVariant>
#include <QUrl>
#include <QWidget>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <thread>
#endif

namespace {

static bool window_matches_source(const QWidget *window,
                                  const QString &source_name)
{
    if (!window || source_name.isEmpty())
        return false;

    const QString title = window->windowTitle();
    qsizetype from = 0;

    /*
     * OBS property windows place the source name inside punctuation/quotes
     * (for example: Properties for 'Camera'). Do not use a plain substring
     * match here: OBS's automatic names such as "Low Latency RTSP" and
     * "Low Latency RTSP 2" would otherwise collide and let one source update
     * another source's open Properties dialog.
     */
    while (from < title.size()) {
        const qsizetype pos =
            title.indexOf(source_name, from, Qt::CaseSensitive);
        if (pos < 0)
            break;

        const qsizetype end = pos + source_name.size();
        const bool before_ok =
            pos == 0 || title.at(pos - 1).isPunct();
        const bool after_ok =
            end == title.size() || title.at(end).isPunct();

        if (before_ok && after_ok)
            return true;

        from = pos + 1;
    }

    return false;
}

static bool is_status_label(const QLabel *label)
{
    if (!label)
        return false;

    const QString text = label->text();
    return text.contains(QStringLiteral("Video:")) &&
           text.contains(QStringLiteral("Dropped:")) &&
           text.contains(QStringLiteral("Reconnects:"));
}

static void apply_info_style(QLabel *label, int info_type)
{
    if (!label)
        return;

    if (info_type == 2)
        label->setProperty("class", QStringLiteral("text-danger"));
    else if (info_type == 1)
        label->setProperty("class", QStringLiteral("text-warning"));
    else
        label->setProperty("class", QVariant());

    if (label->style()) {
        label->style()->unpolish(label);
        label->style()->polish(label);
    }
    label->update();
}

static QString normalized_version(QString version)
{
    version = version.trimmed();
    if (version.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
        version.remove(0, 1);

    const qsizetype suffix = version.indexOf(QLatin1Char('-'));
    if (suffix >= 0)
        version.truncate(suffix);

    return version;
}

static int compare_versions(const QString &left, const QString &right)
{
    const QStringList a = normalized_version(left).split(QLatin1Char('.'));
    const QStringList b = normalized_version(right).split(QLatin1Char('.'));
    const qsizetype count = qMax(a.size(), b.size());

    for (qsizetype i = 0; i < count; ++i) {
        bool a_ok = false;
        bool b_ok = false;
        const int av = i < a.size() ? a.at(i).toInt(&a_ok) : 0;
        const int bv = i < b.size() ? b.at(i).toInt(&b_ok) : 0;

        if (!a_ok && i < a.size())
            return QString::compare(left, right, Qt::CaseInsensitive);
        if (!b_ok && i < b.size())
            return QString::compare(left, right, Qt::CaseInsensitive);

        if (av < bv)
            return -1;
        if (av > bv)
            return 1;
    }

    return 0;
}

static void show_update_check_error(const QString &detail)
{
    QString message =
        QStringLiteral("Could not check GitHub Releases right now.");
    if (!detail.isEmpty())
        message += QStringLiteral("\n\n") + detail;

    QMessageBox::warning(QApplication::activeWindow(),
                         QStringLiteral("Update Plugin"), message);
}

#ifdef _WIN32
struct UpdateCheckResult {
    DWORD status_code = 0;
    QByteArray payload;
    QString error;
};

static QString windows_error_message(DWORD error_code)
{
    wchar_t *buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(
        flags, nullptr, error_code, 0,
        reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);

    QString message;
    if (length > 0 && buffer)
        message = QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed();

    if (buffer)
        LocalFree(buffer);

    if (message.isEmpty())
        message = QStringLiteral("Windows network error %1").arg(error_code);

    return message;
}

static UpdateCheckResult fetch_latest_release()
{
    UpdateCheckResult result;

    HINTERNET session = WinHttpOpen(
        L"OBS-Low-Latency-RTSP-Update-Checker/0.5.2",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        result.error = windows_error_message(GetLastError());
        return result;
    }

    WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);

    HINTERNET connection =
        WinHttpConnect(session, L"api.github.com",
                       INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) {
        result.error = windows_error_message(GetLastError());
        WinHttpCloseHandle(session);
        return result;
    }

    HINTERNET request = WinHttpOpenRequest(
        connection, L"GET",
        L"/repos/Splinxes/obs-low-latency-rtsp-source/releases/latest",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!request) {
        result.error = windows_error_message(GetLastError());
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return result;
    }

    static const wchar_t headers[] =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";

    if (!WinHttpSendRequest(
            request, headers, static_cast<DWORD>(-1L),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        result.error = windows_error_message(GetLastError());
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return result;
    }

    DWORD status_size = sizeof(result.status_code);
    if (!WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &result.status_code,
            &status_size, WINHTTP_NO_HEADER_INDEX)) {
        result.error = windows_error_message(GetLastError());
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return result;
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            result.error = windows_error_message(GetLastError());
            break;
        }

        if (available == 0)
            break;

        const qsizetype old_size = result.payload.size();
        result.payload.resize(old_size + static_cast<qsizetype>(available));

        DWORD bytes_read = 0;
        if (!WinHttpReadData(
                request, result.payload.data() + old_size,
                available, &bytes_read)) {
            result.error = windows_error_message(GetLastError());
            break;
        }

        result.payload.resize(old_size + static_cast<qsizetype>(bytes_read));
        if (bytes_read == 0)
            break;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
}

static void present_update_check_result(const QString &current_version,
                                        const UpdateCheckResult &result)
{
    if (!result.error.isEmpty()) {
        show_update_check_error(result.error);
        return;
    }

    if (result.status_code == 404) {
        QMessageBox::information(
            QApplication::activeWindow(),
            QStringLiteral("Update Plugin"),
            QStringLiteral("No published GitHub Release exists yet."));
        return;
    }

    if (result.status_code < 200 || result.status_code >= 300) {
        show_update_check_error(
            QStringLiteral("GitHub returned HTTP %1.")
                .arg(result.status_code));
        return;
    }

    QJsonParseError parse_error;
    const QJsonDocument document =
        QJsonDocument::fromJson(result.payload, &parse_error);
    if (parse_error.error != QJsonParseError::NoError ||
        !document.isObject()) {
        show_update_check_error(
            QStringLiteral("GitHub returned an unexpected response."));
        return;
    }

    const QJsonObject release = document.object();
    const QString latest_version =
        release.value(QStringLiteral("tag_name")).toString();
    const QString release_url =
        release.value(QStringLiteral("html_url")).toString();

    if (latest_version.isEmpty() || release_url.isEmpty()) {
        show_update_check_error(
            QStringLiteral("The latest release did not contain version information."));
        return;
    }

    if (compare_versions(current_version, latest_version) >= 0) {
        QMessageBox::information(
            QApplication::activeWindow(),
            QStringLiteral("Update Plugin"),
            QStringLiteral("You're up to date.\n\n"
                           "Installed: v%1\nLatest: %2")
                .arg(current_version, latest_version));
        return;
    }

    QMessageBox box(QApplication::activeWindow());
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(QStringLiteral("Update Plugin"));
    box.setText(QStringLiteral("A newer version is available."));
    box.setInformativeText(
        QStringLiteral("Installed: v%1\nAvailable: %2")
            .arg(current_version, latest_version));

    QPushButton *open_release =
        box.addButton(QStringLiteral("Open Update"),
                      QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();

    if (box.clickedButton() == open_release)
        QDesktopServices::openUrl(QUrl(release_url));
}
#endif

} // namespace

extern "C" void llrtsp_ui_update_status(const char *source_name,
                                         const char *status_text,
                                         int info_type)
{
    if (!qApp || !source_name || !*source_name || !status_text)
        return;

    const QString sourceName = QString::fromUtf8(source_name);
    const QString statusText = QString::fromUtf8(status_text);

    QMetaObject::invokeMethod(
        qApp,
        [sourceName, statusText, info_type]() {
            const auto windows = QApplication::topLevelWidgets();
            for (QWidget *window : windows) {
                if (!window || !window->isVisible())
                    continue;

                /*
                 * OBSBasicProperties is OBS's source Properties dialog.
                 * Restrict the search to that window and the current source
                 * name so another source's telemetry can never be overwritten.
                 */
                const char *className = window->metaObject()->className();
                if (!className ||
                    QString::fromLatin1(className) !=
                        QStringLiteral("OBSBasicProperties"))
                    continue;

                if (!window_matches_source(window, sourceName))
                    continue;

                const auto labels = window->findChildren<QLabel *>();
                for (QLabel *label : labels) {
                    if (!is_status_label(label))
                        continue;

                    label->setText(statusText);
                    label->setWordWrap(true);
                    apply_info_style(label, info_type);
                    return;
                }
            }
        },
        Qt::QueuedConnection);
}

extern "C" void llrtsp_ui_copy_text(const char *text)
{
    if (!qApp || !text)
        return;

    const QString clipboardText = QString::fromUtf8(text);
    QMetaObject::invokeMethod(
        qApp,
        [clipboardText]() {
            QClipboard *clipboard = QApplication::clipboard();
            if (clipboard)
                clipboard->setText(clipboardText);
        },
        Qt::QueuedConnection);
}

extern "C" void llrtsp_ui_open_url(const char *url)
{
    if (!qApp || !url || !*url)
        return;

    const QString target = QString::fromUtf8(url);
    QMetaObject::invokeMethod(
        qApp,
        [target]() {
            QDesktopServices::openUrl(QUrl(target));
        },
        Qt::QueuedConnection);
}

extern "C" void llrtsp_ui_update_unifi_hint(const char *source_name,
                                             const char *base_info_text,
                                             const char *hint_text,
                                             bool visible)
{
    if (!qApp || !source_name || !*source_name || !base_info_text ||
        !*base_info_text || !hint_text || !*hint_text)
        return;

    const QString sourceName = QString::fromUtf8(source_name);
    const QString baseInfo = QString::fromUtf8(base_info_text);
    const QString hint = QString::fromUtf8(hint_text);

    QMetaObject::invokeMethod(
        qApp,
        [sourceName, baseInfo, hint, visible]() {
            const auto windows = QApplication::topLevelWidgets();
            for (QWidget *window : windows) {
                if (!window || !window->isVisible())
                    continue;

                const char *className = window->metaObject()->className();
                if (!className ||
                    QString::fromLatin1(className) !=
                        QStringLiteral("OBSBasicProperties"))
                    continue;

                if (!window_matches_source(window, sourceName))
                    continue;

                const auto labels = window->findChildren<QLabel *>();
                for (QLabel *label : labels) {
                    if (!label->text().startsWith(baseInfo,
                                                  Qt::CaseSensitive))
                        continue;

                    const QString desired =
                        visible
                            ? baseInfo + QStringLiteral("\n\n⚠ ") + hint
                            : baseInfo;

                    if (label->text() != desired)
                        label->setText(desired);

                    label->setWordWrap(true);
                    return;
                }
            }
        },
        Qt::QueuedConnection);
}

