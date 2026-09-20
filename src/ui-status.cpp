// SPDX-License-Identifier: GPL-2.0-or-later

#include "ui-status.h"

#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QMetaObject>
#include <QMessageBox>
#include <QStyle>
#include <QString>
#include <QVariant>
#include <QWidget>

namespace {

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

                if (!window->windowTitle().contains(sourceName,
                                                     Qt::CaseSensitive))
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

extern "C" void llrtsp_ui_show_unifi_rtsp_hint(const char *source_name)
{
    if (!qApp || !source_name || !*source_name)
        return;

    const QString sourceName = QString::fromUtf8(source_name);

    QMetaObject::invokeMethod(
        qApp,
        [sourceName]() {
            QWidget *parent = nullptr;
            const auto windows = QApplication::topLevelWidgets();

            for (QWidget *window : windows) {
                if (!window || !window->isVisible())
                    continue;

                const char *className = window->metaObject()->className();
                if (!className ||
                    QString::fromLatin1(className) !=
                        QStringLiteral("OBSBasicProperties"))
                    continue;

                if (!window->windowTitle().contains(sourceName,
                                                     Qt::CaseSensitive))
                    continue;

                parent = window;
                break;
            }

            QMessageBox box(parent);
            box.setIcon(QMessageBox::Warning);
            box.setWindowTitle(
                QStringLiteral("UniFi Protect RTSPS Link Detected"));
            box.setTextFormat(Qt::PlainText);
            box.setText(QStringLiteral(
                "This looks like a secure UniFi Protect RTSPS link. "
                "Low Latency RTSP currently expects the standard RTSP form "
                "for this connection."));
            box.setInformativeText(QStringLiteral(
                "Change only these parts:\n\n"
                "rtsps://  ->  rtsp://\n"
                ":7441     ->  :7447\n"
                "remove    ->  ?enableSrtp\n\n"
                "Keep the stream ID/path exactly the same.\n\n"
                "For privacy, the plugin does not display or modify your "
                "pasted URL."));
            box.setStandardButtons(QMessageBox::Ok);
            box.exec();
        },
        Qt::QueuedConnection);
}

