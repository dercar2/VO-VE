#pragma once

#include <cstdint>

namespace vove::fileops::test {
inline thread_local std::uint64_t current_thread_journal_reads{};
}
