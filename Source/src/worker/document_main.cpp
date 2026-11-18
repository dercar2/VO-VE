#include "document_executor.hpp"

int main() try {
    using namespace vove::worker;
    return run_production_worker(kDocumentWorkerBuildId, capability_bit(Capability::document_handler),
                                 execute_document_job_native);
} catch (...) {
    return static_cast<int>(vove::worker::RuntimeExitCode::io_error);
}
