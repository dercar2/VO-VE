#pragma once

#include "vove/fileops/current_operation_lease.hpp"
#include "vove/fileops/durable_journal.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <cstring>

#ifndef Q_OS_WIN
#include <fcntl.h>
#include <unistd.h>
#endif

namespace vove::ui::transfer_settings {

inline const auto active_key = QStringLiteral("operations/directoryTransferStateV1");
inline constexpr qsizetype maximum_bytes = qsizetype{2} * 1024 * 1024;

inline std::filesystem::path profile_path(const QSettings &settings) {
    auto identity = QDir::fromNativeSeparators(settings.fileName());
    if (identity.isEmpty()) return {};
    auto parent = QFileInfo(identity).absolutePath();
#ifdef Q_OS_WIN
    identity = identity.toCaseFolded();
    if (settings.format() != QSettings::IniFormat)
        parent = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
#endif
    if (parent.isEmpty()) return {};
    const auto digest = QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256);
    return QDir(parent).filesystemPath() / ("vove-directory-transfer-" + digest.toHex().toStdString());
}

inline fileops::CurrentOperationLease lock_profile(const std::filesystem::path &profile) {
    if (profile.empty()) return {};
    std::error_code error;
    return fileops::CurrentOperationLease::try_acquire(profile, error);
}

inline std::filesystem::path archive_path(const std::filesystem::path &profile, const QByteArray &bytes) {
    QCryptographicHash digest(QCryptographicHash::Sha256);
    digest.addData(QByteArray::fromStdString(profile.filename().string()));
    digest.addData(bytes);
    return profile.parent_path() / ("vove-directory-transfer-" + digest.result().toHex().toStdString() + ".archive");
}

// On POSIX a rename is not durable until the containing directory is flushed.
// Include ancestors in case the profile directory itself was newly created.
inline bool sync_parent_directories(const std::filesystem::path &path) {
#ifdef Q_OS_WIN
    static_cast<void>(path);
#else
    for (auto dir = path.parent_path(); !dir.empty(); dir = dir.parent_path()) {
        const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) return false;
        const bool synced = ::fsync(fd) == 0;
        if (::close(fd) != 0 || !synced) return false;
        if (dir == dir.root_path()) break;
    }
#endif
    return true;
}

enum class Active { absent, matches, conflict };

inline Active inspect(QSettings &settings, const QByteArray &expected) {
    settings.sync();
    if (settings.status() != QSettings::NoError) return Active::conflict;
    // remove() also removes descendants. Never clear an unexpected subtree.
    settings.beginGroup(active_key);
    const bool children = !settings.childKeys().isEmpty() || !settings.childGroups().isEmpty();
    settings.endGroup();
    const bool present = settings.contains(active_key);
    const auto value = settings.value(active_key);
    if (children || settings.status() != QSettings::NoError) return Active::conflict;
    if (!present) return Active::absent;
    return value.metaType().id() == QMetaType::QByteArray && value.toByteArray() == expected
               ? Active::matches : Active::conflict;
}

namespace detail {
template <typename SyncParents>
bool archive_with_sync(QSettings &settings, const QByteArray &expected, SyncParents sync_parents) {
    if (expected.isEmpty() || expected.size() > maximum_bytes || !settings.group().isEmpty())
        return false;
    settings.setFallbacksEnabled(false);
    const auto profile = profile_path(settings);
    const auto lock = lock_profile(profile);
    if (!lock.owns_lock()) return false;
    const auto active = inspect(settings, expected);
    if (active == Active::conflict) return false;

    const auto path = archive_path(profile, expected);
    const fileops::DurableJournalStore store(path, maximum_bytes);
    const auto matches = [&expected](const fileops::DurableJournalReadResult &result) {
        return result.ok() && result.payload.size() == static_cast<std::size_t>(expected.size()) &&
               std::memcmp(result.payload.data(), expected.constData(), result.payload.size()) == 0;
    };
    if (store.read_previous_generation().status != fileops::DurableJournalStatus::not_found)
        return false;
    const auto saved = store.read_primary_generation();
    if (!matches(saved)) {
        if (saved.status != fileops::DurableJournalStatus::not_found || active != Active::matches)
            return false;
        const auto bytes = std::as_bytes(std::span(expected.constData(),
                                                   static_cast<std::size_t>(expected.size())));
        if (!store.write(bytes).ok()) return false;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error ||
        !sync_parents(path)) return false;
    const auto current = inspect(settings, expected);
    if (!matches(store.read_primary_generation())) return false;
    if (current == Active::conflict) return false;
    if (current == Active::matches) {
        settings.remove(active_key);
        if (inspect(settings, expected) != Active::absent) return false;
    }
    // A retry can already see an absent key after a failed post-remove sync.
    // Confirm the settings file's directory, not just the archive's directory.
    return sync_parents(QFileInfo(settings.fileName()).filesystemAbsoluteFilePath());
}
} // namespace detail

inline bool archive(QSettings &settings, const QByteArray &expected) {
    return detail::archive_with_sync(settings, expected, sync_parent_directories);
}

} // namespace vove::ui::transfer_settings
