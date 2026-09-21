#pragma once

#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>

#include <algorithm>
#include <array>
#include <optional>

namespace vove::ui {

namespace external_component_detail {

[[nodiscard]] inline bool executable_exists(const QString &path) {
    const QFileInfo file(path);
    return file.isFile() && file.isExecutable();
}

[[nodiscard]] inline QString verified_executable_path(const QString &path) {
    if (!executable_exists(path)) {
        return {};
    }
    const QFileInfo file(path);
    const auto canonical = file.canonicalFilePath();
    return canonical.isEmpty() ? file.absoluteFilePath() : canonical;
}

[[nodiscard]] inline bool ghostscript_version_output_is_valid(QByteArray output) {
    output = output.trimmed();
    if (output.isEmpty() || output.size() > 128) {
        return false;
    }
    const auto first_line = output.split('\n').front().trimmed();
    const auto parts = first_line.split('.');
    if (parts.size() < 2) {
        return false;
    }
    bool major_valid{};
    bool minor_valid{};
    const auto major = parts[0].toUInt(&major_valid);
    static_cast<void>(parts[1].toUInt(&minor_valid));
    return major_valid && minor_valid && major != 0;
}

[[nodiscard]] inline QString verified_ghostscript_path(const QString &path) {
    const auto executable = verified_executable_path(path);
    if (executable.isEmpty()) {
        return {};
    }
    QProcess probe;
    probe.setProgram(executable);
    probe.setArguments({QStringLiteral("--version")});
    probe.setProcessChannelMode(QProcess::MergedChannels);
    probe.start(QIODevice::ReadOnly);
    if (!probe.waitForStarted(1'000)) {
        return {};
    }
    if (!probe.waitForFinished(2'000)) {
        probe.kill();
        static_cast<void>(probe.waitForFinished(1'000));
        return {};
    }
    return probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0 &&
                   ghostscript_version_output_is_valid(probe.readAll())
               ? executable
               : QString{};
}

[[nodiscard]] inline QString ghostscript_under_program_files(const QString &program_files) {
    if (program_files.isEmpty()) {
        return {};
    }
    const QDir versions(QDir(program_files).filePath(QStringLiteral("gs")));
    const auto installations =
        versions.entryInfoList(QStringList{QStringLiteral("gs*")},
                               QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
    for (const auto &installation : installations) {
        const QDir bin(QDir(installation.absoluteFilePath()).filePath(QStringLiteral("bin")));
        for (const auto &name : {QStringLiteral("gswin64c.exe"), QStringLiteral("gswin32c.exe")}) {
            const auto executable = verified_ghostscript_path(bin.filePath(name));
            if (!executable.isEmpty()) {
                return executable;
            }
        }
    }
    return {};
}

#ifdef Q_OS_WIN
[[nodiscard]] inline QString ghostscript_in_windows_registry() {
    const QStringList roots{
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\GPL Ghostscript"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Artifex Ghostscript"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\GPL Ghostscript"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Artifex "
                       "Ghostscript")};
    for (const auto &root : roots) {
        QSettings registry(root, QSettings::NativeFormat);
        for (const auto &version : registry.childGroups()) {
            registry.beginGroup(version);
            const QFileInfo library(registry.value(QStringLiteral("GS_DLL")).toString());
            registry.endGroup();
            const QDir bin = library.dir();
            if (!library.isFile()) {
                continue;
            }
            for (const auto &name :
                 {QStringLiteral("gswin64c.exe"), QStringLiteral("gswin32c.exe")}) {
                const auto executable = verified_ghostscript_path(bin.filePath(name));
                if (!executable.isEmpty()) {
                    return executable;
                }
            }
        }
    }
    return {};
}
#endif

[[nodiscard]] inline QString detect_ghostscript_on_system() {
#ifdef Q_OS_WIN
    const QStringList candidates{QStringLiteral("gswin64c.exe"), QStringLiteral("gswin32c.exe"),
                                 QStringLiteral("gs.exe")};
#else
    const QStringList candidates{QStringLiteral("gs")};
#endif
    for (const auto &candidate : candidates) {
        const auto executable =
            verified_ghostscript_path(QStandardPaths::findExecutable(candidate));
        if (!executable.isEmpty()) {
            return executable;
        }
    }
#ifdef Q_OS_WIN
    if (const auto executable = ghostscript_in_windows_registry(); !executable.isEmpty()) {
        return executable;
    }
    const QStringList roots{qEnvironmentVariable("ProgramW6432"),
                            qEnvironmentVariable("ProgramFiles"),
                            qEnvironmentVariable("ProgramFiles(x86)")};
    for (const auto &root : roots) {
        if (const auto executable = ghostscript_under_program_files(root); !executable.isEmpty()) {
            return executable;
        }
    }
#endif
    return {};
}

[[nodiscard]] inline bool everything_under_program_files(const QString &program_files) {
    if (program_files.isEmpty()) {
        return false;
    }
    const QDir root(program_files);
    const auto installations = root.entryInfoList(QStringList{QStringLiteral("Everything*")},
                                                  QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto &installation : installations) {
        const QDir directory(installation.absoluteFilePath());
        if (executable_exists(directory.filePath(QStringLiteral("Everything.exe"))) ||
            executable_exists(directory.filePath(QStringLiteral("Everything64.exe")))) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool everything_ipc_available();

[[nodiscard]] inline bool detect_everything_on_system() {
#ifdef Q_OS_WIN
    if (everything_ipc_available()) {
        return true;
    }
    const QStringList candidates{QStringLiteral("Everything.exe"),
                                 QStringLiteral("Everything64.exe")};
    for (const auto &candidate : candidates) {
        if (!QStandardPaths::findExecutable(candidate).isEmpty()) {
            return true;
        }
    }
    const QStringList roots{qEnvironmentVariable("ProgramW6432"),
                            qEnvironmentVariable("ProgramFiles"),
                            qEnvironmentVariable("ProgramFiles(x86)")};
    for (const auto &root : roots) {
        if (everything_under_program_files(root)) {
            return true;
        }
    }
#endif
    return false;
}

[[nodiscard]] inline QString line_seed_jp_family() {
    const auto families = QFontDatabase::families();
    const auto found = std::find_if(families.cbegin(), families.cend(), [](const QString &family) {
        return family.startsWith(QStringLiteral("LINE Seed JP"), Qt::CaseInsensitive);
    });
    return found == families.cend() ? QString{} : *found;
}

[[nodiscard]] inline bool detect_line_seed_jp_on_system() {
    return !line_seed_jp_family().isEmpty();
}

[[nodiscard]] inline bool detect_plocate_on_system() {
#ifndef Q_OS_WIN
    return !QStandardPaths::findExecutable(QStringLiteral("plocate")).isEmpty();
#else
    return false;
#endif
}

} // namespace external_component_detail

[[nodiscard]] inline std::optional<QString> &ghostscript_executable_cache() {
    static std::optional<QString> executable;
    return executable;
}

[[nodiscard]] inline QString ghostscript_executable(const bool refresh = false) {
    if (qEnvironmentVariableIsSet("VOVE_GHOSTSCRIPT_EXECUTABLE")) {
        return external_component_detail::verified_executable_path(
            qEnvironmentVariable("VOVE_GHOSTSCRIPT_EXECUTABLE"));
    }
    auto &executable = ghostscript_executable_cache();
    if (refresh) {
        executable = external_component_detail::detect_ghostscript_on_system();
    }
    return executable.value_or(QString{});
}

inline void cache_ghostscript_executable(QString executable) {
    if (qEnvironmentVariableIsSet("VOVE_GHOSTSCRIPT_EXECUTABLE")) {
        return;
    }
    // Keep this cache on the UI thread. Discovery may run elsewhere, but publishing does not.
    ghostscript_executable_cache() = std::move(executable);
}

[[nodiscard]] inline bool ghostscript_available(const bool refresh = false) {
    return !ghostscript_executable(refresh).isEmpty();
}

[[nodiscard]] inline bool bubblewrap_available() {
#ifdef Q_OS_WIN
    return true;
#else
    constexpr std::array<const char *, 3> candidates{"/usr/bin/bwrap", "/bin/bwrap",
                                                      "/usr/local/bin/bwrap"};
    return std::ranges::any_of(candidates, [](const char *candidate) {
        return !external_component_detail::verified_executable_path(QString::fromLatin1(candidate))
                    .isEmpty();
    });
#endif
}

[[nodiscard]] inline bool everything_available(const bool refresh = false) {
    if (qEnvironmentVariableIsSet("VOVE_EVERYTHING_EXECUTABLE")) {
        return external_component_detail::executable_exists(
            qEnvironmentVariable("VOVE_EVERYTHING_EXECUTABLE"));
    }
    static std::optional<bool> available;
    if (refresh || !available) {
        available = external_component_detail::detect_everything_on_system();
    }
    return *available;
}

[[nodiscard]] inline bool line_seed_jp_available(const bool refresh = false) {
    if (qEnvironmentVariableIsSet("VOVE_LINE_SEED_JP_AVAILABLE")) {
        return qEnvironmentVariableIntValue("VOVE_LINE_SEED_JP_AVAILABLE") != 0;
    }
    static std::optional<bool> available;
    if (refresh || !available) {
        available = external_component_detail::detect_line_seed_jp_on_system();
    }
    return *available;
}

[[nodiscard]] inline bool plocate_available(const bool refresh = false) {
    if (qEnvironmentVariableIsSet("VOVE_PLOCATE_EXECUTABLE")) {
        return external_component_detail::executable_exists(
            qEnvironmentVariable("VOVE_PLOCATE_EXECUTABLE"));
    }
    static std::optional<bool> available;
    if (refresh || !available) {
        available = external_component_detail::detect_plocate_on_system();
    }
    return *available;
}

} // namespace vove::ui
