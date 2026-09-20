// SPDX-License-Identifier: GPL-2.0-or-later

#include "ui-status.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QLabel>
#include <QMetaObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStyle>
#include <QString>
#include <QVariant>
#include <QUrl>
#include <QWidget>

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

extern "C" void llrtsp_ui_check_for_update(const char *current_version)
{
    if (!qApp || !current_version || !*current_version)
        return;

    const QString currentVersion = QString::fromUtf8(current_version);

    QMetaObject::invokeMethod(
        qApp,
        [currentVersion]() {
            static const QUrl releasesApi(
                QStringLiteral("https://api.github.com/repos/"
                               "Splinxes/obs-low-latency-rtsp-source/"
                               "releases/latest"));

            auto *manager = new QNetworkAccessManager(qApp);
            QNetworkRequest request(releasesApi);
            request.setRawHeader("Accept", "application/vnd.github+json");
            request.setRawHeader("User-Agent",
                                 "OBS-Low-Latency-RTSP-Update-Checker");
            request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");

            QNetworkReply *reply = manager->get(request);
            QObject::connect(
                reply, &QNetworkReply::finished, qApp,
                [reply, manager, currentVersion]() {
                    const QNetworkReply::NetworkError networkError =
                        reply->error();
                    const QString networkDetail = reply->errorString();
                    const QByteArray payload = reply->readAll();

                    reply->deleteLater();
                    manager->deleteLater();

                    if (networkError != QNetworkReply::NoError) {
                        show_update_check_error(networkDetail);
                        return;
                    }

                    QJsonParseError parseError;
                    const QJsonDocument document =
                        QJsonDocument::fromJson(payload, &parseError);
                    if (parseError.error != QJsonParseError::NoError ||
                        !document.isObject()) {
                        show_update_check_error(
                            QStringLiteral("GitHub returned an unexpected response."));
                        return;
                    }

                    const QJsonObject release = document.object();
                    const QString latestVersion =
                        release.value(QStringLiteral("tag_name")).toString();
                    const QString releaseUrl =
                        release.value(QStringLiteral("html_url")).toString();

                    if (latestVersion.isEmpty() || releaseUrl.isEmpty()) {
                        show_update_check_error(
                            QStringLiteral("The latest release did not contain version information."));
                        return;
                    }

                    if (compare_versions(currentVersion, latestVersion) >= 0) {
                        QMessageBox::information(
                            QApplication::activeWindow(),
                            QStringLiteral("Update Plugin"),
                            QStringLiteral("You're up to date.\n\n"
                                           "Installed: v%1\nLatest: %2")
                                .arg(currentVersion, latestVersion));
                        return;
                    }

                    QMessageBox box(QApplication::activeWindow());
                    box.setIcon(QMessageBox::Information);
                    box.setWindowTitle(QStringLiteral("Update Plugin"));
                    box.setText(
                        QStringLiteral("A newer version is available."));
                    box.setInformativeText(
                        QStringLiteral("Installed: v%1\nAvailable: %2")
                            .arg(currentVersion, latestVersion));

                    QPushButton *openRelease =
                        box.addButton(QStringLiteral("Open Update"),
                                      QMessageBox::AcceptRole);
                    box.addButton(QMessageBox::Cancel);
                    box.exec();

                    if (box.clickedButton() == openRelease)
                        QDesktopServices::openUrl(QUrl(releaseUrl));
                });
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

