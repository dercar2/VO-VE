#pragma once

#include <initializer_list>
#include <memory>
#include <string_view>
#include <utility>

namespace vove::ui {

using DiagnosticField = std::pair<std::string_view, std::string_view>;

class DiagnosticSink final {
  public:
    static DiagnosticSink &instance() noexcept;

    DiagnosticSink(const DiagnosticSink &) = delete;
    DiagnosticSink &operator=(const DiagnosticSink &) = delete;

    [[nodiscard]] bool activate_from_environment() noexcept;
    [[nodiscard]] bool active() const noexcept;
    void record(std::string_view event,
                std::initializer_list<DiagnosticField> fields = {}) noexcept;
    void heartbeat() noexcept;
    void shutdown() noexcept;

  private:
    DiagnosticSink() = default;
    ~DiagnosticSink();

    struct State;
    std::unique_ptr<State> state_;
};

} // namespace vove::ui
