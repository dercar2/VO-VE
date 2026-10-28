#pragma once

namespace vove::ui {

// Captured once per UI update. These gates do not replace submission-time validation.
struct OperationAvailability {
    bool mutation_busy{};
    bool pending_rename{};
    bool external_drop_pending{};
    bool drop_probe_idle{true};
    bool trash_confirmation{};
    bool batch_recovery{};
    bool delete_recovery{};
    bool transfer_recovery{};
    bool trash_recovery{};
    bool directory_recovery{};
    bool journal_blocked{};
    bool unfinished_journal{};
    bool trash_view{};
    bool global_search{};
    bool recursive_view{};
    bool catalog_ready{};

    [[nodiscard]] bool mutation_idle() const noexcept {
        return !mutation_busy && !pending_rename;
    }
    [[nodiscard]] bool drop_idle() const noexcept {
        return !external_drop_pending && drop_probe_idle;
    }
    [[nodiscard]] bool recovery_clear() const noexcept {
        return !batch_recovery && !transfer_recovery && !trash_recovery &&
               !directory_recovery && !journal_blocked;
    }
    [[nodiscard]] bool selection_mutation_allowed() const noexcept {
        // Permanent-delete recovery was deliberately not part of rename/delete UI gating.
        return mutation_idle() && drop_idle() && recovery_clear() && !trash_confirmation &&
               !trash_view && !global_search && (!recursive_view || catalog_ready);
    }
    [[nodiscard]] bool create_directory_allowed() const noexcept {
        // Unlike selection mutations, this gate does not wait for drop/confirmation state.
        return mutation_idle() && recovery_clear() && !delete_recovery &&
               !trash_view && !global_search && !recursive_view;
    }
    [[nodiscard]] bool trash_catalog_allowed() const noexcept {
        return mutation_idle() && drop_idle();
    }
    [[nodiscard]] bool unfinished_operation_allowed() const noexcept {
        return !mutation_busy && unfinished_journal;
    }
    [[nodiscard]] bool selection_context_visible(bool has_objects) const noexcept {
        return has_objects && !global_search && (!recursive_view || catalog_ready);
    }
};

} // namespace vove::ui
