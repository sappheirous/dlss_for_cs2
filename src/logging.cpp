#include "logging.hpp"

#include <utility>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include "settings.hpp"

void initialize_logging() {
    auto logger =
        spdlog::rotating_logger_mt("cs2-dlss", (data_directory() / "cs2_dlss.log").string(), 4 * 1024 * 1024, 3);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [thread %t] %v");
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));
}
