#include "raster_executor.hpp"

#include <QCoreApplication>

int main(int argc, char **argv) try {
    using namespace vove::worker;
    QCoreApplication application(argc, argv);
#ifdef _WIN32
    QCoreApplication::setLibraryPaths({QCoreApplication::applicationDirPath()});
#endif
    return run_production_worker(
        kRasterWorkerBuildId, capability_bit(Capability::read_only_profile_token) |
                                  capability_bit(Capability::raster_handler),
        execute_raster_job_native, execute_gif_animation_native);
} catch (...) {
    return static_cast<int>(vove::worker::RuntimeExitCode::io_error);
}
