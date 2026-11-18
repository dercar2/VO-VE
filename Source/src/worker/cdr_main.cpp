#include "cdr_executor.hpp"

int main() try {
    using namespace vove::worker;
    return run_production_worker(kCdrWorkerBuildId, capability_bit(Capability::cdr_handler),
                                 execute_cdr_job_native);
} catch (...) {
    return static_cast<int>(vove::worker::RuntimeExitCode::io_error);
}
