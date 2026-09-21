#include "object_transfer_queue.hpp"

#include "vove/fileops/current_operation_journal.hpp"
#include "vove/fileops/current_operation_lease.hpp"
#include "vove/fileops/basic_directory_transfer_protocol.hpp"
#include "vove/core/reserved_names.hpp"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <thread>

namespace vove::ui {
namespace {
using namespace fileops;
constexpr std::size_t maximum_objects = 1024;
thread_local const void *active_queue_callback{};

struct CallbackScope {
    const void *previous{active_queue_callback};
    explicit CallbackScope(const void *state) {
        active_queue_callback = state;
    }
    ~CallbackScope() {
        active_queue_callback = previous;
    }
};

QString text_path(const std::filesystem::path &path) {
    const auto utf8 = path.generic_u8string();
    return QString::fromUtf8(reinterpret_cast<const char *>(utf8.data()), qsizetype(utf8.size()));
}
std::filesystem::path native(const QString &text) {
    const auto bytes = QDir::toNativeSeparators(text).toUtf8();
    return std::filesystem::path(std::u8string_view(
        reinterpret_cast<const char8_t *>(bytes.constData()), std::size_t(bytes.size())));
}
QString key(const std::filesystem::path &path) {
    auto text = QDir::fromNativeSeparators(text_path(path));
#ifdef Q_OS_WIN
    if (text.startsWith("//?/UNC/", Qt::CaseInsensitive))
        text = "//" + text.mid(8);
    else if (text.startsWith("//?/"))
        text = text.mid(4);
    text = text.normalized(QString::NormalizationForm_C).toCaseFolded();
#endif
    return QDir::cleanPath(text);
}
bool contains_path(const std::filesystem::path &parent, const std::filesystem::path &child) {
    auto prefix = key(parent);
    if (!prefix.endsWith('/'))
        prefix += '/';
    return key(child).startsWith(prefix);
}
SourceSnapshot snapshot(const core::DirectoryEntry &entry) {
    return {entry.size_bytes, entry.modified_unix_ns, entry.source_revision_utf8};
}
QJsonObject encode_snapshot(const SourceSnapshot &value) {
    return {{"bytes", QString::number(value.size_bytes)},
            {"modified", QString::number(value.modified_unix_ns)},
            {"revision", QString::fromStdString(value.source_revision_utf8)}};
}
SourceSnapshot decode_snapshot(const QJsonObject &value) {
    bool size_ok{}, date_ok{};
    SourceSnapshot result{value.value("bytes").toString().toULongLong(&size_ok),
                          value.value("modified").toString().toLongLong(&date_ok),
                          value.value("revision").toString().toStdString()};
    if (!size_ok || !date_ok || result.source_revision_utf8.empty()) {
        throw std::runtime_error("Invalid object snapshot in transfer queue");
    }
    return result;
}
struct Item {
    std::filesystem::path source, destination;
    core::EntryKind kind{};
    SourceSnapshot source_snapshot, overwrite;
    std::string source_parent, destination_parent;
};
struct Plan {
    FileTransferKind kind{};
    std::vector<Item> items;
    std::size_t index{};
    // 0: ready; 1: child may have started; 2/3: child success/failure durably acknowledged.
    int phase{};
    std::uint64_t operation_id{};
    std::filesystem::path directory_manifest;
    bool cancelled{};
    std::string failure_detail;
    OperationStatus failure_status{OperationStatus::success};
};

bool bound_manifest_path(const Plan &plan, const std::filesystem::path &path) {
    if (plan.index >= plan.items.size() || !native(key(path)).is_absolute() ||
        std::ranges::any_of(path, [](const auto &part) { return part == "." || part == ".."; }) ||
        path.filename() != "manifest")
        return false;
    const auto &item = plan.items[plan.index];
    if (item.kind != core::EntryKind::directory ||
        key(path.parent_path().parent_path()) != key(item.destination.parent_path()))
        return false;
    const auto prefix = core::kTransferDestinationFilenamePrefix;
    const auto expected =
        QString::fromUtf8(reinterpret_cast<const char *>(prefix.data()), qsizetype(prefix.size())) +
        QString::number(plan.operation_id + plan.index, 16) + "-0-";
    const auto name = text_path(path.parent_path().filename());
    const auto token =
        name.mid(expected.size(), qsizetype(kDirectoryTransferOwnershipTokenBytes * 2));
    return name.startsWith(expected) && name.endsWith(".tree.control") &&
           name.size() == expected.size() + token.size() + 13 &&
           token.size() == qsizetype(kDirectoryTransferOwnershipTokenBytes * 2) &&
           std::ranges::all_of(
               token, [](QChar c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool clean_directory_failure(const BasicDirectoryTransferStatus status) {
    switch (status) {
    case BasicDirectoryTransferStatus::invalid_request:
    case BasicDirectoryTransferStatus::not_found:
    case BasicDirectoryTransferStatus::conflict:
    case BasicDirectoryTransferStatus::source_changed:
    case BasicDirectoryTransferStatus::permission_denied:
    case BasicDirectoryTransferStatus::file_in_use:
    case BasicDirectoryTransferStatus::unsupported:
        return true;
    default:
        return false;
    }
}
} // namespace

struct ObjectTransferQueue::State : std::enable_shared_from_this<State> {
    FileTransferCoordinatorOptions options;
    platform::DirectoryServiceOptions catalog_options;
    CurrentOperationJournalStore journal;
    std::atomic_bool running{}, stopping{};
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t active_workers{};
    std::optional<FileTransferConflictDecision> decision;
    bool awaiting_decision{};
    Plan plan;
    OperationStatus reported_status{OperationStatus::success};
    std::shared_ptr<FileTransferCoordinator> file_child;
    std::shared_ptr<FileTransferService> directory_child;

    State(FileTransferCoordinatorOptions value, platform::DirectoryServiceOptions catalog)
        : options(std::move(value)), catalog_options(std::move(catalog)),
          journal(options.journal_path) {}

    std::filesystem::path child_path() const {
        auto result = options.journal_path;
        result += ".object-child";
        return result;
    }
    void save() {
        QJsonArray items;
        for (const auto &item : plan.items) {
            items.append(QJsonObject{
                {"source", text_path(item.source)},
                {"destination", text_path(item.destination)},
                {"kind", int(item.kind)},
                {"snapshot", encode_snapshot(item.source_snapshot)},
                {"overwrite", item.overwrite.source_revision_utf8.empty()
                                  ? QJsonObject{}
                                  : encode_snapshot(item.overwrite)},
                {"sourceParent", QString::fromStdString(item.source_parent)},
                {"destinationParent", QString::fromStdString(item.destination_parent)}});
        }
        const auto data =
            QJsonDocument(QJsonObject{{"schema", 2},
                                      {"kind", int(plan.kind)},
                                      {"index", qint64(plan.index)},
                                      {"phase", plan.phase},
                                      {"manifest", text_path(plan.directory_manifest)},
                                      {"cancelled", plan.cancelled},
                                      {"failure", QString::fromStdString(plan.failure_detail)},
                                      {"failureStatus", int(plan.failure_status)},
                                      {"operation", QString::number(plan.operation_id)},
                                      {"items", items}})
                .toJson(QJsonDocument::Compact);
        if (!journal
                 .write(CurrentOperationKind::object_transfer,
                        {reinterpret_cast<const std::byte *>(data.constData()),
                         std::size_t(data.size())})
                 .ok()) {
            throw std::runtime_error("Cannot save object transfer queue");
        }
    }
    void load() {
        const auto record = journal.read();
        if (!record.ok() || record.encoding != CurrentOperationJournalEncoding::typed ||
            record.kind != CurrentOperationKind::object_transfer)
            throw std::runtime_error("Object queue unavailable");
        const auto document = QJsonDocument::fromJson(
            QByteArray(reinterpret_cast<const char *>(record.payload.data()),
                       qsizetype(record.payload.size())));
        const auto value = document.object();
        const auto items = value.value("items").toArray();
        const auto kind = value.value("kind").toInt(-1);
        const auto index = value.value("index").toInteger(-1);
        const auto phase = value.value("phase").toInt(-1);
        bool id_ok{};
        const auto id = value.value("operation").toString().toULongLong(&id_ok);
        const auto schema = value.value("schema").toInt();
        if ((schema != 1 && schema != 2) || (kind != 0 && kind != 1) || items.isEmpty() ||
            items.size() > qsizetype(maximum_objects) || index < 0 || index > items.size() ||
            phase < 0 || phase > 3 || !id_ok || id == 0 ||
            id > std::numeric_limits<std::uint64_t>::max() - std::uint64_t(items.size()) ||
            (index == items.size() && phase != 0)) {
            throw std::runtime_error("Invalid object transfer queue");
        }
        plan = {.kind = FileTransferKind(kind),
                .items = {},
                .index = std::size_t(index),
                .phase = phase,
                .operation_id = id,
                .directory_manifest = {},
                .cancelled = schema == 1 && phase == 3,
                .failure_detail = {}};
        if (schema == 2) {
            if (!value.value("manifest").isString() || !value.value("cancelled").isBool() ||
                !value.value("failure").isString())
                throw std::runtime_error("Invalid terminal queue state");
            plan.directory_manifest = native(value.value("manifest").toString());
            plan.cancelled = value.value("cancelled").toBool();
            plan.failure_detail = value.value("failure").toString().toStdString();
            if (value.contains("failureStatus")) {
                const auto status = value.value("failureStatus").toInt(-1);
                if (status < 0 || status > int(OperationStatus::file_in_use))
                    throw std::runtime_error("Invalid queue failure status");
                plan.failure_status = OperationStatus(status);
            }
            if (phase != 3 && (plan.cancelled || !plan.failure_detail.empty() ||
                               plan.failure_status != OperationStatus::success))
                throw std::runtime_error("Unexpected terminal queue state");
        }
        QSet<QString> sources, destinations;
        for (const auto &entry : items) {
            const auto item = entry.toObject();
            const auto item_kind = item.value("kind").toInt(-1);
            Item decoded{.source = native(item.value("source").toString()),
                         .destination = native(item.value("destination").toString()),
                         .kind = core::EntryKind(item_kind),
                         .source_snapshot = decode_snapshot(item.value("snapshot").toObject()),
                         .overwrite = {},
                         .source_parent = item.value("sourceParent").toString().toStdString(),
                         .destination_parent =
                             item.value("destinationParent").toString().toStdString()};
            if (!decoded.source.is_absolute() || !decoded.destination.is_absolute() ||
                decoded.source.filename().empty() || decoded.destination.filename().empty() ||
                (item_kind != 0 && item_kind != 1) || decoded.source_parent.empty() ||
                decoded.destination_parent.empty() || sources.contains(key(decoded.source)) ||
                destinations.contains(key(decoded.destination)) ||
                key(decoded.source) == key(decoded.destination) ||
                contains_path(decoded.source, decoded.destination)) {
                throw std::runtime_error("Invalid paths in object transfer queue");
            }
            if (!item.value("overwrite").toObject().isEmpty())
                decoded.overwrite = decode_snapshot(item.value("overwrite").toObject());
            sources.insert(key(decoded.source));
            destinations.insert(key(decoded.destination));
            plan.items.push_back(std::move(decoded));
        }
        if (!plan.directory_manifest.empty() &&
            ((phase != 1 && phase != 2) || !bound_manifest_path(plan, plan.directory_manifest))) {
            throw std::runtime_error(
                "Directory manifest does not belong to the current queue item");
        }
    }
    struct Probe {
        std::optional<core::DirectoryEntry> entry;
        std::string revision;
    };
    struct ProbeError : std::runtime_error {
        catalog::CatalogErrorKind kind;
        explicit ProbeError(const catalog::CatalogError &error)
            : std::runtime_error(error.message_utf8), kind(error.kind) {}
    };
    Probe probe(const std::filesystem::path &path, bool directory = false) {
        platform::DirectoryService service(catalog_options);
        constexpr catalog::RequestGeneration generation = 1;
        service.submit({.generation = generation,
                        .path = directory ? path : path.parent_path(),
                        .maximum_entries = 1,
                        .exact_entry_path = directory ? std::filesystem::path{} : path});
        Probe result;
        bool final{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!final || !service.idle()) {
            if (stopping || std::chrono::steady_clock::now() >= deadline) {
                service.cancel(generation);
                throw std::runtime_error("Object check interrupted or timed out");
            }
            if (auto batch = service.poll()) {
                if (batch->error &&
                    !(batch->error.kind == catalog::CatalogErrorKind::not_found && !directory)) {
                    throw ProbeError(batch->error);
                }
                for (auto &entry : batch->entries) {
                    if (!directory &&
                        key(native(QString::fromStdString(entry.path_utf8))) == key(path))
                        result.entry = std::move(entry);
                }
                if (!batch->directory_revision_utf8.empty())
                    result.revision = batch->directory_revision_utf8;
                final = batch->is_final;
            } else
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (directory && result.revision.empty())
            throw std::runtime_error("Cannot verify the transfer directory");
        return result;
    }
    FileTransferConflictDecision ask(FileTransferConflict question, bool overwrite,
                                     const Conflict &callback, bool copy = true) {
        {
            std::scoped_lock lock(mutex);
            decision.reset();
            awaiting_decision = true;
        }
        try {
            const CallbackScope scope(this);
            callback(std::move(question), overwrite, copy);
        } catch (...) {
            std::scoped_lock lock(mutex);
            awaiting_decision = false;
            throw;
        }
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return stopping || decision.has_value(); });
        awaiting_decision = false;
        if (stopping)
            throw std::runtime_error("Object transfer stopped");
        return *decision;
    }
    bool prepare(FileTransferKind kind, const std::vector<core::DirectoryEntry> &entries,
                 const std::filesystem::path &destination, const Conflict &conflict) {
        if (entries.empty() || entries.size() > maximum_objects || !destination.is_absolute())
            throw std::runtime_error("Invalid transfer selection");
        auto id = std::uint64_t(std::chrono::system_clock::now().time_since_epoch().count());
        plan = {.kind = kind,
                .items = {},
                .index = 0,
                .phase = 0,
                .operation_id = id == 0 ? 1 : id,
                .directory_manifest = {},
                .cancelled = false,
                .failure_detail = {}};
        const auto destination_parent = probe(destination, true).revision;
        QSet<QString> sources, targets;
        FileTransferConflict question;
        std::vector<std::size_t> collisions;
        bool overwrite_allowed = true;
        for (const auto &entry : entries) {
            const auto source = native(QString::fromStdString(entry.path_utf8)).lexically_normal();
            const auto target = destination / source.filename();
            if (!source.is_absolute() || source.filename().empty() ||
                sources.contains(key(source)) || targets.contains(key(target)) ||
                (key(source) == key(target) && kind != FileTransferKind::copy) ||
                contains_path(source, target)) {
                throw std::runtime_error("Source and destination paths overlap or are duplicated");
            }
            for (const auto &other : plan.items) {
                if (contains_path(other.source, source) || contains_path(source, other.source))
                    throw std::runtime_error("Selected objects contain one another");
            }
            const auto observed = probe(source).entry;
            // Catalog metadata can lag NTFS handle metadata. Selection names an object;
            // this exact snapshot binds its revision before conflict decisions or mutations.
            const auto typed_identity = entry.source_revision_utf8.starts_with("win-file128:") ||
                                        entry.source_revision_utf8.starts_with("win-smb64:") ||
                                        entry.source_revision_utf8.starts_with("posix2:");
            if (!observed || observed->kind != entry.kind ||
                !(same_source_revision(entry.source_revision_utf8,
                                       observed->source_revision_utf8) ||
                  (typed_identity && same_object_identity(entry.source_revision_utf8,
                                                          observed->source_revision_utf8)))) {
                throw std::runtime_error("Selected object changed before transfer");
            }
            Item item{.source = source,
                      .destination = target,
                      .kind = entry.kind,
                      .source_snapshot = snapshot(*observed),
                      .overwrite = {},
                      .source_parent = probe(source.parent_path(), true).revision,
                      .destination_parent = destination_parent};
            // A same-path copy always needs a new name. Reuse its verified source
            // snapshot so a disappearing source cannot bypass this conflict.
            const auto existing = key(source) == key(target) ? observed : probe(target).entry;
            if (existing) {
                collisions.push_back(plan.items.size());
                question.items.push_back({source, target});
                item.overwrite = snapshot(*existing);
                overwrite_allowed = overwrite_allowed && entry.kind == core::EntryKind::file &&
                                    existing->kind == core::EntryKind::file && key(source) != key(target);
            }
            sources.insert(key(source));
            targets.insert(key(target));
            plan.items.push_back(std::move(item));
        }
        if (!question.items.empty()) {
            question.source = question.items.front().source;
            question.destination = question.items.front().destination;
            const auto choice = ask(question, overwrite_allowed, conflict);
            if (choice == FileTransferConflictDecision::cancel)
                return false;
            if (choice == FileTransferConflictDecision::overwrite && !overwrite_allowed)
                throw std::runtime_error("Replacing an existing folder is not supported");
            if (choice == FileTransferConflictDecision::create_copy) {
                for (const auto index : collisions) {
                    auto &item = plan.items[index];
                    const auto extension = item.kind == core::EntryKind::file
                                               ? text_path(item.source.extension())
                                               : QString{};
                    const auto stem = item.kind == core::EntryKind::file
                                          ? text_path(item.source.stem())
                                          : text_path(item.source.filename());
                    bool found{};
                    for (int count = 1; count <= 10000; ++count) {
                        const auto suffix =
                            QString::fromUtf8("_копия") +
                            (count == 1 ? QString{} : QStringLiteral("_%1").arg(count));
                        auto candidate = destination / native(stem + suffix + extension);
                        if (!targets.contains(key(candidate)) && !probe(candidate).entry) {
                            item.destination = std::move(candidate);
                            item.overwrite = {};
                            targets.insert(key(item.destination));
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        throw std::runtime_error("No available copy name within the limit");
                }
            }
        }
        if (std::ranges::any_of(plan.items, [](const auto &item) {
                return key(item.source) == key(item.destination);
            })) {
            throw std::runtime_error("Copy destination still names its source");
        }
        save();
        return true;
    }
    void bind_file_journal() {
        const auto loaded = CurrentOperationJournalStore(child_path()).read();
        if (loaded.status == DurableJournalStatus::not_found)
            return;
        FileTransferTransaction transaction;
        std::string detail;
        const auto &item = plan.items.at(plan.index);
        if (!loaded.ok() || loaded.kind != CurrentOperationKind::file_transfer ||
            !decode_file_transfer_transaction(loaded.payload, transaction, detail) ||
            transaction.items.size() != 1 || transaction.kind != plan.kind ||
            key(transaction.items.front().source) != key(item.source) ||
            key(transaction.items.front().destination) != key(item.destination) ||
            !same_source_revision(
                transaction.items.front().original_source_snapshot.source_revision_utf8,
                item.source_snapshot.source_revision_utf8)) {
            throw std::runtime_error("Child journal does not belong to the selected object");
        }
    }
    bool terminal_failure(std::string detail,
                          OperationStatus status = OperationStatus::io_error) {
        plan.phase = 3;
        plan.cancelled = false;
        plan.failure_detail = std::move(detail);
        plan.failure_status = status;
        plan.directory_manifest.clear();
        save();
        return false;
    }
    bool verify_directory_parents(const Item &item) {
        try {
            const auto source_parent = probe(item.source.parent_path(), true).revision;
            const auto destination_parent = probe(item.destination.parent_path(), true).revision;
            if (!same_object_identity(source_parent, item.source_parent) ||
                !same_object_identity(destination_parent, item.destination_parent)) {
                return terminal_failure("Transfer directory was replaced");
            }
        } catch (const ProbeError &error) {
            if (error.kind != catalog::CatalogErrorKind::not_found)
                throw;
            return terminal_failure("Transfer directory no longer exists");
        }
        return true;
    }
    void report_progress(const Progress &progress, FileTransferProgressUpdate value) {
        const CallbackScope scope(this);
        progress(std::move(value));
    }
    bool run_file(const Progress &progress, const Conflict &conflict) {
        auto child_options = options;
        child_options.journal_path = child_path();
        child_options.before_journal_retirement = [this](const FileTransferTransaction &transaction,
                                                         std::string &detail) {
            try {
                plan.phase =
                    transaction.phase == FileTransferPhase::completed && !transaction.cancelled ? 2
                                                                                                : 3;
                plan.cancelled = transaction.cancelled;
                plan.failure_detail =
                    plan.phase == 3 ? transaction.failure_detail_utf8 : std::string{};
                plan.failure_status =
                    plan.phase == 3 ? transaction.failure_status : OperationStatus::success;
                save();
                return !options.before_journal_retirement ||
                       options.before_journal_retirement(transaction, detail);
            } catch (const std::exception &error) {
                detail = error.what();
                return false;
            }
        };
        auto child = std::make_shared<FileTransferCoordinator>(child_options);
        {
            std::scoped_lock lock(mutex);
            file_child = child;
        }
        bind_file_journal();
        std::mutex result_mutex;
        std::condition_variable result_ready;
        std::optional<FileTransferResult> result;
        const auto done = [&](FileTransferResult value) {
            std::scoped_lock lock(result_mutex);
            result = std::move(value);
            result_ready.notify_all();
        };
        const auto report = [&](FileTransferProgressUpdate value) {
            value.completed = plan.index;
            value.total = plan.items.size();
            report_progress(progress, std::move(value));
        };
        // A collision that appeared after preflight requires a fresh decision; keep the explicit
        // destination binding.
        const auto late_conflict = [&](FileTransferConflict value) {
            const auto choice = ask(std::move(value), true, conflict, false);
            child->resolve_conflict(choice);
        };
        auto &item = plan.items[plan.index];
        bool submitted{};
        if (child->owns_recovery())
            submitted = child->resume(report, done, late_conflict);
        else {
            if (!verify_directory_parents(item))
                return false;
            submitted = child->start(plan.kind,
                                     {{.path = item.source,
                                       .destination = item.destination,
                                       .snapshot = item.source_snapshot,
                                       .source_parent_revision_utf8 = item.source_parent,
                                       .destination_parent_revision_utf8 = item.destination_parent,
                                       .authorized_overwrite_destination = item.overwrite}},
                                     report, done, late_conflict);
        }
        if (!submitted)
            throw std::runtime_error("Cannot start the file transfer child");
        {
            std::unique_lock lock(result_mutex);
            while (!result) {
                result_ready.wait_for(lock, std::chrono::milliseconds(100));
                if (stopping) {
                    lock.unlock();
                    child->stop();
                    throw std::runtime_error("Transfer stopped");
                }
            }
        }
        while (child->busy())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        {
            std::scoped_lock lock(mutex);
            file_child.reset();
        }
        if (!result->ok()) {
            reported_status = plan.phase == 3 && plan.failure_status != OperationStatus::success
                                  ? plan.failure_status : result->operation_status;
            if (plan.phase == 3 && !child->owns_recovery())
                return false;
            throw std::runtime_error(result->detail_utf8.empty() ? "File transfer requires recovery"
                                                                 : result->detail_utf8);
        }
        return true;
    }
    bool run_directory(bool recovering, const Progress &progress) {
        const auto item = plan.items.at(plan.index);
        auto service = std::make_shared<FileTransferService>(options.file_transfers);
        service->retain_accepted_completions_during_stop();
        {
            std::scoped_lock lock(mutex);
            directory_child = service;
        }
        auto command = recovering ? (plan.directory_manifest.empty()
                                         ? BasicDirectoryTransferCommand::discover_and_resume
                                         : BasicDirectoryTransferCommand::resume)
                                  : (plan.kind == FileTransferKind::move
                                         ? BasicDirectoryTransferCommand::move
                                         : BasicDirectoryTransferCommand::copy);
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto fresh = command == BasicDirectoryTransferCommand::copy ||
                               command == BasicDirectoryTransferCommand::move;
            if (fresh && !verify_directory_parents(item))
                return false;
            if (stopping)
                throw std::runtime_error("Transfer stopped");
            std::mutex result_mutex;
            std::condition_variable result_ready;
            std::optional<BasicDirectoryTransferResult> result;
            const auto id = plan.operation_id + plan.index;
            const bool submitted = service->submit_directory(
                {.request_id = id,
                 .operation_id = id,
                 .command = command,
                 .source = item.source,
                 .destination = item.destination,
                 .manifest_path = command == BasicDirectoryTransferCommand::resume
                                      ? plan.directory_manifest
                                      : std::filesystem::path{},
                 .expected_source_revision_utf8 = item.source_snapshot.source_revision_utf8},
                [&](const BasicDirectoryTransferProgress &value) {
                    if (value.phase == BasicDirectoryTransferPhase::completed) {
                        plan.phase = 2;
                        save();
                    }
                    report_progress(progress, {.completed = plan.index,
                                               .total = plan.items.size(),
                                               .item_index = plan.index,
                                               .bytes_written = value.completed_bytes,
                                               .bytes_total = value.total_bytes,
                                               .source = item.source,
                                               .destination = item.destination});
                },
                [&](BasicDirectoryTransferResult value) {
                    std::scoped_lock lock(result_mutex);
                    result = std::move(value);
                    result_ready.notify_all();
                });
            if (!submitted)
                throw std::runtime_error("Cannot start the folder transfer child");
            {
                std::unique_lock lock(result_mutex);
                while (!result) {
                    result_ready.wait_for(lock, std::chrono::milliseconds(100));
                    if (stopping) {
                        lock.unlock();
                        service->stop();
                        throw std::runtime_error("Transfer stopped");
                    }
                }
            }
            while (service->busy())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            reported_status = result->status == BasicDirectoryTransferStatus::file_in_use
                                  ? OperationStatus::file_in_use : OperationStatus::io_error;
            if (result->recovery_available) {
                if (!result->manifest_path.empty()) {
                    if (!bound_manifest_path(plan, result->manifest_path))
                        throw std::runtime_error("Folder result has a mismatched manifest path");
                    plan.directory_manifest = result->manifest_path;
                    save();
                }
                throw std::runtime_error(result->detail_utf8.empty()
                                             ? "Folder transfer requires recovery"
                                             : result->detail_utf8);
            }
            if (result->ok() ||
                (plan.phase == 2 && result->status == BasicDirectoryTransferStatus::not_found))
                return true;
            if (attempt == 0 && command == BasicDirectoryTransferCommand::discover_and_resume &&
                plan.phase == 1 && result->status == BasicDirectoryTransferStatus::not_found) {
                command = plan.kind == FileTransferKind::move ? BasicDirectoryTransferCommand::move
                                                              : BasicDirectoryTransferCommand::copy;
                continue;
            }
            if (fresh && plan.phase == 1 && clean_directory_failure(result->status)) {
                return terminal_failure(result->detail_utf8.empty()
                                            ? "Folder transfer failed before mutation"
                                            : result->detail_utf8, reported_status);
            }
            throw std::runtime_error(result->detail_utf8.empty()
                                         ? "Folder transfer requires recovery"
                                         : result->detail_utf8);
        }
        throw std::runtime_error("Folder recovery retry limit reached");
    }
    ObjectTransferResult execute(const Progress &progress, const Conflict &conflict) {
        while (plan.index < plan.items.size()) {
            if (stopping)
                throw std::runtime_error("Transfer stopped");
            const auto recovering = plan.phase != 0;
            if (!recovering) {
                plan.phase = 1;
                save();
            }
            if (plan.items[plan.index].kind == core::EntryKind::file) {
                const auto child_record = CurrentOperationJournalStore(child_path()).read();
                if (plan.phase >= 2 && child_record.status == DurableJournalStatus::not_found) {
                    if (plan.phase == 3)
                        break;
                } else if (!run_file(progress, conflict))
                    break;
            } else if (plan.phase == 3 || !run_directory(recovering, progress))
                break;
            if (plan.phase != 2)
                throw std::runtime_error(
                    "Child transfer has no durable completion acknowledgement");
            ++plan.index;
            plan.phase = 0;
            plan.directory_manifest.clear();
            save();
        }
        const auto success = plan.index == plan.items.size();
        if (!journal.remove().ok())
            throw std::runtime_error("Cannot retire completed object queue");
        return {.success = success,
                .cancelled = !success && plan.cancelled,
                .recovery = false,
                .operation_status = success ? OperationStatus::success : plan.failure_status,
                .completed = plan.index,
                .total = plan.items.size(),
                .detail = QString::fromStdString(plan.failure_detail),
                .moved_sources = {},
                .published_paths = {}};
    }
    bool launch(std::optional<std::vector<core::DirectoryEntry>> sources, FileTransferKind kind,
                std::filesystem::path destination, Progress progress, Conflict conflict,
                Completion completion) {
        if (!progress || !conflict || !completion)
            return false;
        {
            std::scoped_lock lock(mutex);
            if (stopping || running)
                return false;
            running = true;
            reported_status = OperationStatus::success;
            ++active_workers;
        }
        const auto failed_launch = [this] {
            {
                std::scoped_lock lock(mutex);
                running = false;
                --active_workers;
            }
            changed.notify_all();
        };
        std::thread worker;
        try {
            std::error_code error;
            auto lease = CurrentOperationLease::try_acquire(options.journal_path, error);
            if (!lease.owns_lock()) {
                failed_launch();
                return false;
            }
            const auto self = shared_from_this();
            worker = std::thread(
                [self, sources = std::move(sources), kind, destination = std::move(destination),
                 progress = std::move(progress), conflict = std::move(conflict),
                 completion = std::move(completion), lease = std::move(lease)]() mutable {
                    struct WorkerCompletion {
                        State &state;
                        ~WorkerCompletion() {
                            {
                                std::scoped_lock lock(state.mutex);
                                --state.active_workers;
                            }
                            state.changed.notify_all();
                        }
                    } worker_completion{*self};
                    ObjectTransferResult result;
                    bool plan_loaded = false;
                    const auto failed = [&](const char *detail) noexcept {
                        result.recovery = true;
                        try {
                            result.detail = QString::fromUtf8(detail);
                            result.recovery =
                                self->journal.read().status != DurableJournalStatus::not_found;
                            result.completed = plan_loaded ? self->plan.index : 0;
                            result.total = plan_loaded ? self->plan.items.size() : 0;
                            result.operation_status = plan_loaded && self->plan.phase == 3 &&
                                self->plan.failure_status != OperationStatus::success
                                    ? self->plan.failure_status : self->reported_status;
                        } catch (...) {
                            // Keep recovery blocked if even failure reporting cannot read the
                            // journal.
                        }
                    };
                    try {
                        if (sources) {
                            if (self->journal.read().status != DurableJournalStatus::not_found ||
                                CurrentOperationJournalStore(self->child_path()).read().status !=
                                    DurableJournalStatus::not_found)
                                throw std::runtime_error(
                                    "An unfinished operation must be recovered first");
                            if (!self->prepare(kind, *sources, destination, conflict))
                                result.cancelled = true;
                            else {
                                plan_loaded = true;
                                result = self->execute(progress, conflict);
                            }
                        } else {
                            self->load();
                            plan_loaded = true;
                            result = self->execute(progress, conflict);
                        }
                    } catch (const std::exception &error) {
                        failed(error.what());
                    } catch (...) {
                        failed("Object transfer failed unexpectedly");
                    }
                    if (plan_loaded) {
                        const auto acknowledged = std::min(self->plan.items.size(), self->plan.index +
                            (self->plan.phase == 2 ? std::size_t{1} : std::size_t{0}));
                        for (std::size_t i = 0; i < acknowledged; ++i) {
                            result.published_paths.push_back(text_path(self->plan.items[i].destination));
                            if (self->plan.kind == FileTransferKind::move)
                                result.moved_sources.push_back(text_path(self->plan.items[i].source));
                        }
                    }
                    // Child destruction drains their callbacks before the parent releases its
                    // operation lease.
                    self->file_child.reset();
                    self->directory_child.reset();
                    lease.release();
                    {
                        std::scoped_lock lock(self->mutex);
                        self->running = false;
                    }
                    self->changed.notify_all();
                    if (!self->stopping) {
                        const CallbackScope scope(self.get());
                        try {
                            completion(std::move(result));
                        } catch (...) {
                            // A caller callback cannot terminate the queue worker or prevent stop
                            // from draining it.
                        }
                    }
                });
        } catch (...) {
            failed_launch();
            return false;
        }
        try {
            worker.detach();
        } catch (...) {
            worker.join();
        }
        return true;
    }
};

ObjectTransferQueue::ObjectTransferQueue(FileTransferCoordinatorOptions options,
                                         platform::DirectoryServiceOptions catalog)
    : state_(std::make_shared<State>(std::move(options), std::move(catalog))) {}
ObjectTransferQueue::~ObjectTransferQueue() {
    stop();
}
bool ObjectTransferQueue::start(FileTransferKind kind, std::vector<core::DirectoryEntry> sources,
                                std::filesystem::path destination, Progress progress,
                                Conflict conflict, Completion completion) {
    return state_->launch(std::move(sources), kind, std::move(destination), std::move(progress),
                          std::move(conflict), std::move(completion));
}
bool ObjectTransferQueue::resume(Progress progress, Conflict conflict, Completion completion) {
    return state_->launch(std::nullopt, FileTransferKind::copy, {}, std::move(progress),
                          std::move(conflict), std::move(completion));
}
void ObjectTransferQueue::resolve_conflict(FileTransferConflictDecision decision) {
    {
        std::scoped_lock lock(state_->mutex);
        if (state_->awaiting_decision)
            state_->decision = decision;
    }
    state_->changed.notify_all();
}
bool ObjectTransferQueue::busy() const {
    return state_->running;
}
void ObjectTransferQueue::stop() {
    std::unique_lock lock(state_->mutex);
    state_->stopping = true;
    state_->changed.notify_all();
    if (active_queue_callback == state_.get())
        return;
    state_->changed.wait(lock, [&] { return state_->active_workers == 0; });
}

} // namespace vove::ui
