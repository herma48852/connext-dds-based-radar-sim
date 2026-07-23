// ============================================================================
// Radar simulation application - AESA SPY-6 class phased array radar.
//
// Internal components communicate exclusively over DDS topics (same domain):
//   Radar.BeamScheduler        -> Radar/BeamCommand         (100 Hz)
//   Radar.Beamformer           <- BeamCommand, CalibrationStatus
//                              -> Radar/BeamPatternStatus    (20 Hz)
//   Radar.DetectionProcessor   <- BeamCommand, BeamPatternStatus
//                              -> Radar/RawReturn (1 kHz), Radar/DetectionEvent
//   Radar.TrackManager         -> Radar/TargetTrack          (10 Hz)
//   Radar.CalibrationMonitor   -> Radar/CalibrationStatus    (1 Hz + changes)
//   Radar.CommandHandler       <- Radar/SystemCommand        (WaitSet)
//   Radar.ShipINS              -> Ship/ShipPosition          (10 Hz)
//   Radar.CommandConsole       -> Radar/SystemCommand        (UI scenarios)
//   Radar.HMI-UI               <- TargetTrack, DetectionEvent,
//                                 ShipPosition, CalibrationStatus,
//                                 BeamPatternStatus (display)
//
// Usage: radar_app [--domain N]     (default domain 0)
// ============================================================================

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <exception>
#include <memory>
#include <windows.h>
#include <dbghelp.h>
#endif

// Crash diagnostics: dump a backtrace to stderr on fatal signals so the
// failing thread is identifiable without a debugger attached (POSIX only).
#if defined(__unix__) || defined(__APPLE__)
#include <cstdio>
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
namespace {
void crash_handler(int sig) {
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    char buf[128];
    const int len = std::snprintf(buf, sizeof(buf),
        "\n[radar_app] fatal signal %d (%s) — backtrace:\n", sig,
        sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : "SIGABRT");
    if (len > 0) (void)!::write(STDERR_FILENO, buf, static_cast<size_t>(len));
    ::backtrace_symbols_fd(frames, n, STDERR_FILENO);
    ::_exit(128 + sig);
}
void install_crash_handler() {
    std::signal(SIGSEGV, crash_handler);
    std::signal(SIGBUS,  crash_handler);
    std::signal(SIGABRT, crash_handler);
}
} // namespace
#elif defined(_WIN32)
namespace {
void print_windows_stack() noexcept {
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    if (!SymInitialize(process, nullptr, TRUE))
        return;

    void* frames[64]{};
    const USHORT count = CaptureStackBackTrace(0, 64, frames, nullptr);
    auto storage = std::make_unique<unsigned char[]>(
        sizeof(SYMBOL_INFO) + MAX_SYM_NAME);
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage.get());
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    for (USHORT i = 0; i < count; ++i) {
        const DWORD64 address = reinterpret_cast<DWORD64>(frames[i]);
        DWORD64 displacement = 0;
        if (SymFromAddr(process, address, &displacement, symbol)) {
            std::fprintf(stderr, "  #%u %s + 0x%llx\n", i, symbol->Name,
                         static_cast<unsigned long long>(displacement));
        } else {
            std::fprintf(stderr, "  #%u 0x%llx\n", i,
                         static_cast<unsigned long long>(address));
        }
    }
    SymCleanup(process);
}

[[noreturn]] void terminate_handler() noexcept {
    std::fprintf(stderr, "[radar_app] std::terminate invoked");
    if (const auto error = std::current_exception()) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& e) {
            std::fprintf(stderr, ": %s", e.what());
        } catch (...) {
            std::fprintf(stderr, ": non-standard exception");
        }
    }
    std::fprintf(stderr, "\n");
    print_windows_stack();
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 134);
    std::abort();
}

void abort_handler(int) noexcept {
    std::fprintf(stderr, "[radar_app] SIGABRT\n");
    print_windows_stack();
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 134);
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* details) noexcept {
    const auto code = details->ExceptionRecord->ExceptionCode;
    const auto address = details->ExceptionRecord->ExceptionAddress;
    std::fprintf(stderr, "[radar_app] unhandled Windows exception 0x%08lx at %p\n",
                 static_cast<unsigned long>(code), address);

    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    if (SymInitialize(process, nullptr, TRUE)) {
        auto storage = std::make_unique<unsigned char[]>(
            sizeof(SYMBOL_INFO) + MAX_SYM_NAME);
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage.get());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        if (SymFromAddr(process, reinterpret_cast<DWORD64>(address),
                        &displacement, symbol)) {
            std::fprintf(stderr, "  %s + 0x%llx\n", symbol->Name,
                         static_cast<unsigned long long>(displacement));
        }

#if defined(_M_X64)
        CONTEXT context = *details->ContextRecord;
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        for (unsigned int i = 0; i < 32; ++i) {
            if (i != 0 && !StackWalk64(
                    IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(),
                    &frame, &context, nullptr, SymFunctionTableAccess64,
                    SymGetModuleBase64, nullptr)) {
                break;
            }
            displacement = 0;
            if (SymFromAddr(process, frame.AddrPC.Offset,
                            &displacement, symbol)) {
                std::fprintf(stderr, "  #%u %s + 0x%llx\n", i,
                             symbol->Name,
                             static_cast<unsigned long long>(displacement));
            } else {
                std::fprintf(stderr, "  #%u 0x%llx\n", i,
                             static_cast<unsigned long long>(
                                 frame.AddrPC.Offset));
            }
        }
#endif
        SymCleanup(process);
    }
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

void install_crash_handler() {
    std::set_terminate(terminate_handler);
    std::signal(SIGABRT, abort_handler);
    SetUnhandledExceptionFilter(unhandled_exception_filter);
}
} // namespace
#else
void install_crash_handler() {}
#endif

#include "CommandConsole.hpp"
#include "DataBus.hpp"
#include "Log.hpp"
#include "ShipSimulator.hpp"
#include "SimClock.hpp"
#include "components/Beamformer.hpp"
#include "components/BeamScheduler.hpp"
#include "components/CalibrationMonitor.hpp"
#include "components/CommandHandler.hpp"
#include "components/DetectionProcessor.hpp"
#include "components/HmiUi.hpp"
#include "components/TrackManager.hpp"
#include "ui/UiApp.hpp"

namespace {
std::atomic<bool> g_running{true};
void on_sigint(int) { g_running.store(false); }
} // namespace

int main(int argc, char** argv) {
    radds::disable_monitoring_lib(); // no monitoring DPs (see DdsSupport)
    install_crash_handler();
    int32_t domain = 0;
    bool headless = false;
    bool no_dispose = false;
    bool gl_throttle = false;
    bool no_titlebar = false;
    int swap_interval = 1;
    double run_seconds = 0.0;
    std::string stop_file;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--domain") == 0 && i + 1 < argc)
            domain = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--headless") == 0)
            headless = true;
        else if (std::strcmp(argv[i], "--no-dispose") == 0)
            no_dispose = true;
        else if (std::strcmp(argv[i], "--gl-throttle") == 0)
            gl_throttle = true;
        else if (std::strcmp(argv[i], "--no-titlebar") == 0)
            no_titlebar = true;
        else if (std::strcmp(argv[i], "--swap-interval") == 0 && i + 1 < argc)
            swap_interval = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--run-seconds") == 0 && i + 1 < argc)
            run_seconds = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--stop-file") == 0 && i + 1 < argc)
            stop_file = argv[++i];
        else if (std::strcmp(argv[i], "--help") == 0) {
            RADAR_LOG << "radar_app [--domain N] [--headless] [--no-dispose]\n"
                         "          [--gl-throttle] [--swap-interval N]\n"
                         "          [--run-seconds N] [--stop-file PATH]\n"
                         "  --headless       components only (no window); crash-bisect\n"
                         "                   soak: same DDS traffic, zero GLFW/ImGui/AppKit\n"
                         "  --no-dispose     TrackManager never calls dispose_instance\n"
                         "  --gl-throttle    upload the B-scope texture every 4th frame\n"
                         "                   (15 Hz); tests GL driver load as crash suspect\n"
                         "  --swap-interval  legacy knob; no-op with the Metal renderer\n"
                         "                   to 30 fps (default 1 = vsync 60 fps)\n"
                         "  --no-titlebar    undecorated window; removes native titlebar\n"
                         "  --run-seconds    stop cleanly after N seconds (automation)\n"
                         "  --stop-file      stop cleanly when PATH appears\n";
            return 0;
        }
    }

    if (run_seconds < 0.0) {
        std::cerr << "--run-seconds must be non-negative\n";
        return 2;
    }

    const auto process_started = std::chrono::steady_clock::now();
    const auto stop_requested = [&] {
        if (run_seconds > 0.0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now()
                                          - process_started).count() >= run_seconds)
            return true;
        if (!stop_file.empty()) {
            std::error_code ec;
            if (std::filesystem::exists(stop_file, ec) && !ec)
                return true;
        }
        return false;
    };

    radar::SimClock::start();
    RADAR_LOG << "[radar_app] starting on DDS domain " << domain << "\n";

    radar::app::DataBus bus;
    if (no_dispose) {
        bus.dispose_enabled.store(false);
        RADAR_LOG << "[radar_app] --no-dispose: dispose_instance disabled\n";
    }

    radar::app::ShipSimulator      ship(domain, bus);
    radar::app::BeamScheduler      scheduler(domain, bus);
    radar::app::Beamformer         beamformer(domain);
    radar::app::DetectionProcessor processor(domain, bus);
    radar::app::TrackManager       tracker(domain, bus);
    radar::app::CalibrationMonitor calibration(domain, bus);
    radar::app::CommandHandler     commands(domain, bus);
    radar::app::CommandConsole     console(domain);
    radar::app::HmiUi              hmi(domain, bus);

    // Order matters only for startup cosmetics; DDS discovery handles wiring.
    ship.start();
    calibration.start();
    beamformer.start();
    scheduler.start();
    processor.start();
    tracker.start();
    commands.start();
    console.start();
    hmi.start();

    int rc = 0;
    if (headless) {
        // Soak test for the SIGSEGV investigation: identical components,
        // participants and DDS traffic as the full app, but no GLFW window,
        // no ImGui and no AppKit. If this survives for hours while the
        // windowed build dies after ~1 minute, the corruptor lives in the
        // UI/windowing layer, not in the DDS/component layer.
        std::signal(SIGINT, on_sigint);
        RADAR_LOG << "[radar_app] headless soak mode; Ctrl+C to stop\n";
        auto next_heartbeat = std::chrono::steady_clock::now();
        while (g_running.load() && !stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (std::chrono::steady_clock::now() >= next_heartbeat) {
                next_heartbeat += std::chrono::seconds(10);
                RADAR_LOG << "[radar_app] alive t="
                          << radar::SimClock::sim_millis() / 1000 << " s\n";
            }
        }
    } else {
        radar::ui::UiApp app(bus, console);
        app.set_stop_requested(stop_requested);
        if (gl_throttle) {
            app.set_bscope_upload_decimation(4);
            RADAR_LOG << "[radar_app] --gl-throttle: B-scope upload at 15 Hz\n";
        }
        if (no_titlebar) {
            app.set_undecorated(true);
            RADAR_LOG << "[radar_app] --no-titlebar: undecorated window\n";
        }
        if (swap_interval != 1) {
            app.set_swap_interval(swap_interval);
            RADAR_LOG << "[radar_app] swap interval " << swap_interval << "\n";
        }
        rc = app.run(); // blocks until the window closes
    }

    RADAR_LOG << "[radar_app] shutting down\n";
    hmi.stop();
    console.stop();
    commands.stop();
    tracker.stop();
    processor.stop();
    scheduler.stop();
    beamformer.stop();
    calibration.stop();
    ship.stop();
    RADAR_LOG << "[radar_app] components stopped\n";
    return rc;
}
