// SPDX-License-Identifier: GPL-2.0-or-later

#include "ui-status.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QStringList>
#include <QUrl>

#include <atomic>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#endif

namespace {

static constexpr qint64 MAX_UPDATE_DOWNLOAD_BYTES = 128LL * 1024LL * 1024LL;
static std::atomic_bool update_in_progress{false};

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

static void show_update_error(const QString &message)
{
    QMessageBox::warning(QApplication::activeWindow(),
                         QStringLiteral("Update Plugin"), message);
}

#ifdef _WIN32

struct HttpResult {
    DWORD status_code = 0;
    QByteArray payload;
    QString error;
};

struct ReleaseInfo {
    QString latest_version;
    QString release_url;
    QString zip_name;
    QUrl zip_url;
    QUrl checksum_url;
};

struct StagedUpdate {
    QString error;
    QString latest_version;
    QString package_path;
    QString updater_script_path;
    QString expected_hash;
    QString obs_exe;
    qint64 obs_pid = 0;
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
        message =
            QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed();

    if (buffer)
        LocalFree(buffer);

    if (message.isEmpty())
        message = QStringLiteral("Windows network error %1").arg(error_code);

    return message;
}

static HttpResult fetch_https_url(const QUrl &url)
{
    HttpResult result;

    if (!url.isValid() ||
        url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0 ||
        url.host().isEmpty()) {
        result.error = QStringLiteral("The update URL was not a valid HTTPS URL.");
        return result;
    }

    const std::wstring host = url.host().toStdWString();
    QString target = url.path(QUrl::FullyEncoded);
    if (target.isEmpty())
        target = QStringLiteral("/");
    const QString query = url.query(QUrl::FullyEncoded);
    if (!query.isEmpty())
        target += QStringLiteral("?") + query;
    const std::wstring request_target = target.toStdWString();

    HINTERNET session = WinHttpOpen(
        L"OBS-Low-Latency-RTSP-Updater/0.5.5",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        result.error = windows_error_message(GetLastError());
        return result;
    }

    WinHttpSetTimeouts(session, 5000, 5000, 5000, 15000);

    const int requested_port = url.port(INTERNET_DEFAULT_HTTPS_PORT);
    const INTERNET_PORT port =
        static_cast<INTERNET_PORT>(requested_port > 0
                                       ? requested_port
                                       : INTERNET_DEFAULT_HTTPS_PORT);

    HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0);
    if (!connection) {
        result.error = windows_error_message(GetLastError());
        WinHttpCloseHandle(session);
        return result;
    }

    HINTERNET request = WinHttpOpenRequest(
        connection, L"GET", request_target.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!request) {
        result.error = windows_error_message(GetLastError());
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return result;
    }

    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY,
                     &redirect_policy, sizeof(redirect_policy));

    static const wchar_t headers[] =
        L"Accept: */*\r\n"
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

        const qint64 new_size =
            static_cast<qint64>(result.payload.size()) +
            static_cast<qint64>(available);
        if (new_size > MAX_UPDATE_DOWNLOAD_BYTES) {
            result.error =
                QStringLiteral("The update download exceeded the 128 MB safety limit.");
            break;
        }

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

static bool is_expected_release_asset_url(const QString &url)
{
    static const QString prefix = QStringLiteral(
        "https://github.com/Splinxes/obs-low-latency-rtsp-source/"
        "releases/download/");
    return url.startsWith(prefix, Qt::CaseInsensitive);
}

static bool valid_sha256_hex(const QByteArray &hash)
{
    if (hash.size() != 64)
        return false;

    for (const char c : hash) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        const bool upper = c >= 'A' && c <= 'F';
        if (!digit && !lower && !upper)
            return false;
    }

    return true;
}

static bool parse_release_info(const QJsonObject &release,
                               ReleaseInfo *info,
                               QString *error)
{
    if (!info)
        return false;

    info->latest_version =
        release.value(QStringLiteral("tag_name")).toString();
    info->release_url =
        release.value(QStringLiteral("html_url")).toString();

    if (info->latest_version.isEmpty() || info->release_url.isEmpty()) {
        if (error)
            *error = QStringLiteral(
                "The latest release did not contain version information.");
        return false;
    }

    const QString clean_version = normalized_version(info->latest_version);
    info->zip_name =
        QStringLiteral("low-latency-rtsp-v%1-windows-x64.zip")
            .arg(clean_version);
    const QString checksum_name = info->zip_name + QStringLiteral(".sha256");

    const QJsonArray assets =
        release.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue &value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name =
            asset.value(QStringLiteral("name")).toString();
        const QString browser_url =
            asset.value(QStringLiteral("browser_download_url")).toString();

        if (!is_expected_release_asset_url(browser_url))
            continue;

        if (name == info->zip_name)
            info->zip_url = QUrl(browser_url);
        else if (name == checksum_name)
            info->checksum_url = QUrl(browser_url);
    }

    if (!info->zip_url.isValid() || !info->checksum_url.isValid()) {
        if (error)
            *error = QStringLiteral(
                "The release is missing the Windows update package or checksum.");
        return false;
    }

    return true;
}

static StagedUpdate download_and_stage_update(const ReleaseInfo &release)
{
    StagedUpdate staged;
    staged.latest_version = release.latest_version;

    const HttpResult zip = fetch_https_url(release.zip_url);
    if (!zip.error.isEmpty()) {
        staged.error =
            QStringLiteral("Could not download the update package.\n\n%1")
                .arg(zip.error);
        return staged;
    }
    if (zip.status_code < 200 || zip.status_code >= 300) {
        staged.error =
            QStringLiteral("Downloading the update package returned HTTP %1.")
                .arg(zip.status_code);
        return staged;
    }

    const HttpResult checksum = fetch_https_url(release.checksum_url);
    if (!checksum.error.isEmpty()) {
        staged.error =
            QStringLiteral("Could not download the update checksum.\n\n%1")
                .arg(checksum.error);
        return staged;
    }
    if (checksum.status_code < 200 || checksum.status_code >= 300) {
        staged.error =
            QStringLiteral("Downloading the update checksum returned HTTP %1.")
                .arg(checksum.status_code);
        return staged;
    }

    const QByteArray expected_hash =
        checksum.payload.trimmed().left(64).toLower();
    if (!valid_sha256_hex(expected_hash)) {
        staged.error =
            QStringLiteral("GitHub returned an invalid SHA-256 checksum.");
        return staged;
    }

    const QByteArray actual_hash =
        QCryptographicHash::hash(zip.payload, QCryptographicHash::Sha256)
            .toHex()
            .toLower();
    if (actual_hash != expected_hash) {
        staged.error = QStringLiteral(
            "The downloaded update failed SHA-256 verification. "
            "No files were changed.");
        return staged;
    }

    const QString staging_root =
        QDir(QDir::tempPath()).filePath(
            QStringLiteral("low-latency-rtsp-update-%1")
                .arg(QCoreApplication::applicationPid()));
    QDir staging_dir(staging_root);
    if (staging_dir.exists() && !staging_dir.removeRecursively()) {
        staged.error =
            QStringLiteral("Could not clear the previous update staging folder.");
        return staged;
    }

    if (!QDir().mkpath(staging_root)) {
        staged.error =
            QStringLiteral("Could not create the update staging folder.");
        return staged;
    }

    staged.package_path =
        QDir(staging_root).filePath(release.zip_name);
    QFile package_file(staged.package_path);
    if (!package_file.open(QIODevice::WriteOnly) ||
        package_file.write(zip.payload) != zip.payload.size()) {
        staged.error =
            QStringLiteral("Could not save the downloaded update package.");
        QDir(staging_root).removeRecursively();
        return staged;
    }
    package_file.close();

    const QString program_data = qEnvironmentVariable("ProgramData");
    if (program_data.isEmpty()) {
        staged.error =
            QStringLiteral("Windows ProgramData could not be located.");
        QDir(staging_root).removeRecursively();
        return staged;
    }

    const QString installed_updater =
        QDir(program_data).filePath(
            QStringLiteral(
                "obs-studio/plugins/low-latency-rtsp/"
                "data/updater/Install-Update.ps1"));
    staged.updater_script_path =
        QDir(staging_root).filePath(
            QStringLiteral("Install-Update.ps1"));

    if (!QFile::exists(installed_updater)) {
        staged.error = QStringLiteral(
            "The self-update helper is not installed. "
            "Install this plugin version once using Install-Plugin.ps1 "
            "or the release installer, then retry.");
        QDir(staging_root).removeRecursively();
        return staged;
    }

    QFile::remove(staged.updater_script_path);
    if (!QFile::copy(installed_updater, staged.updater_script_path)) {
        staged.error =
            QStringLiteral("Could not stage the update installer helper.");
        QDir(staging_root).removeRecursively();
        return staged;
    }

    staged.expected_hash = QString::fromLatin1(expected_hash);
    staged.obs_exe = QCoreApplication::applicationFilePath();
    staged.obs_pid = QCoreApplication::applicationPid();
    return staged;
}

static QString quoted_windows_argument(QString value)
{
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(value);
}

static bool launch_update_helper(const StagedUpdate &staged,
                                 QString *error)
{
    const QString system_root = qEnvironmentVariable("SystemRoot");
    const QString powershell =
        QDir(system_root).filePath(
            QStringLiteral(
                "System32/WindowsPowerShell/v1.0/powershell.exe"));

    if (system_root.isEmpty() || !QFile::exists(powershell)) {
        if (error)
            *error = QStringLiteral("Windows PowerShell could not be located.");
        return false;
    }

    const QString parameters =
        QStringLiteral(
            "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass "
            "-File %1 -PackageZip %2 -ExpectedHash %3 "
            "-ObsPid %4 -ObsExe %5")
            .arg(quoted_windows_argument(
                     QDir::toNativeSeparators(staged.updater_script_path)),
                 quoted_windows_argument(
                     QDir::toNativeSeparators(staged.package_path)),
                 quoted_windows_argument(staged.expected_hash),
                 QString::number(staged.obs_pid),
                 quoted_windows_argument(
                     QDir::toNativeSeparators(staged.obs_exe)));

    const std::wstring powershell_w =
        QDir::toNativeSeparators(powershell).toStdWString();
    const std::wstring parameters_w = parameters.toStdWString();

    HINSTANCE launched = ShellExecuteW(
        nullptr, L"runas", powershell_w.c_str(),
        parameters_w.c_str(), nullptr, SW_HIDE);

    const auto result = reinterpret_cast<INT_PTR>(launched);
    if (result <= 32) {
        if (error) {
            *error = QStringLiteral(
                "The updater was not started. "
                "Administrator approval may have been cancelled.");
        }
        return false;
    }

    return true;
}

static void begin_download_and_install(const ReleaseInfo &release)
{
    if (update_in_progress.exchange(true)) {
        QMessageBox::information(
            QApplication::activeWindow(),
            QStringLiteral("Update Plugin"),
            QStringLiteral("An update operation is already in progress."));
        return;
    }

    auto *progress = new QProgressDialog(
        QStringLiteral("Downloading and verifying %1...")
            .arg(release.latest_version),
        QString(), 0, 0, QApplication::activeWindow());
    progress->setWindowTitle(QStringLiteral("Update Plugin"));
    progress->setCancelButton(nullptr);
    progress->setWindowModality(Qt::ApplicationModal);
    progress->setMinimumDuration(0);
    progress->show();

    QPointer<QProgressDialog> progress_guard(progress);
    QApplication *application = qApp;

    std::thread([application, progress_guard, release]() {
        const StagedUpdate staged = download_and_stage_update(release);

        if (!application)
            return;

        QMetaObject::invokeMethod(
            application,
            [progress_guard, staged]() {
                update_in_progress.store(false);

                if (progress_guard) {
                    progress_guard->close();
                    progress_guard->deleteLater();
                }

                if (!staged.error.isEmpty()) {
                    show_update_error(staged.error);
                    return;
                }

                QMessageBox confirm(QApplication::activeWindow());
                confirm.setIcon(QMessageBox::Information);
                confirm.setWindowTitle(QStringLiteral("Update Plugin"));
                confirm.setText(
                    QStringLiteral("%1 is downloaded and verified.")
                        .arg(staged.latest_version));
                confirm.setInformativeText(
                    QStringLiteral(
                        "Install it now? OBS Studio will close, "
                        "the plugin will be replaced, and OBS will reopen."));
                QPushButton *install =
                    confirm.addButton(
                        QStringLiteral("Install & Restart OBS"),
                        QMessageBox::AcceptRole);
                confirm.addButton(QMessageBox::Cancel);
                confirm.exec();

                if (confirm.clickedButton() != install)
                    return;

                QString launch_error;
                if (!launch_update_helper(staged, &launch_error))
                    show_update_error(launch_error);
            },
            Qt::QueuedConnection);
    }).detach();
}

static void present_update_check_result(const QString &current_version,
                                        const HttpResult &result)
{
    if (!result.error.isEmpty()) {
        show_update_error(
            QStringLiteral("Could not check GitHub Releases right now.\n\n%1")
                .arg(result.error));
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
        show_update_error(
            QStringLiteral("GitHub returned HTTP %1.")
                .arg(result.status_code));
        return;
    }

    QJsonParseError parse_error;
    const QJsonDocument document =
        QJsonDocument::fromJson(result.payload, &parse_error);
    if (parse_error.error != QJsonParseError::NoError ||
        !document.isObject()) {
        show_update_error(
            QStringLiteral("GitHub returned an unexpected response."));
        return;
    }

    const QJsonObject release_object = document.object();
    const QString latest_version =
        release_object.value(QStringLiteral("tag_name")).toString();
    const QString release_url =
        release_object.value(QStringLiteral("html_url")).toString();

    if (latest_version.isEmpty() || release_url.isEmpty()) {
        show_update_error(
            QStringLiteral(
                "The latest release did not contain version information."));
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

    ReleaseInfo release;
    QString package_error;
    const bool package_ready =
        parse_release_info(release_object, &release, &package_error);

    QMessageBox box(QApplication::activeWindow());
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(QStringLiteral("Update Plugin"));
    box.setText(QStringLiteral("A newer version is available."));

    if (package_ready) {
        box.setInformativeText(
            QStringLiteral(
                "Installed: v%1\nAvailable: %2\n\n"
                "The Windows package will be downloaded and "
                "SHA-256 verified before OBS closes.")
                .arg(current_version, latest_version));

        QPushButton *download =
            box.addButton(QStringLiteral("Download & Install"),
                          QMessageBox::AcceptRole);
        QPushButton *open_release =
            box.addButton(QStringLiteral("Open Release"),
                          QMessageBox::ActionRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();

        if (box.clickedButton() == download)
            begin_download_and_install(release);
        else if (box.clickedButton() == open_release)
            QDesktopServices::openUrl(QUrl(release.release_url));
        return;
    }

    box.setInformativeText(
        QStringLiteral(
            "Installed: v%1\nAvailable: %2\n\n%3")
            .arg(current_version, latest_version, package_error));
    QPushButton *open_release =
        box.addButton(QStringLiteral("Open Release"),
                      QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();

    if (box.clickedButton() == open_release)
        QDesktopServices::openUrl(QUrl(release_url));
}

#endif // _WIN32

} // namespace

extern "C" void llrtsp_ui_check_for_update(const char *current_version)
{
    if (!qApp || !current_version || !*current_version)
        return;

    const QString currentVersion = QString::fromUtf8(current_version);

#ifdef _WIN32
    if (update_in_progress.load()) {
        QMessageBox::information(
            QApplication::activeWindow(),
            QStringLiteral("Update Plugin"),
            QStringLiteral("An update operation is already in progress."));
        return;
    }

    QApplication *application = qApp;
    std::thread([application, currentVersion]() {
        const HttpResult result = fetch_https_url(
            QUrl(QStringLiteral(
                "https://api.github.com/repos/"
                "Splinxes/obs-low-latency-rtsp-source/"
                "releases/latest")));

        if (!application)
            return;

        QMetaObject::invokeMethod(
            application,
            [currentVersion, result]() {
                present_update_check_result(currentVersion, result);
            },
            Qt::QueuedConnection);
    }).detach();
#else
    QDesktopServices::openUrl(
        QUrl(QStringLiteral(
            "https://github.com/Splinxes/"
            "obs-low-latency-rtsp-source/releases")));
#endif
}
