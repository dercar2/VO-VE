#pragma once

#include "vove/fileops/file_operation.hpp"

namespace vove::fileops::detail {

[[nodiscard]] OperationResult merge_reconciliation(OperationResult attempted,
                                                   OperationResult observed);

} // namespace vove::fileops::detail
