#include "BeamScheduler.hpp"

#include <array>
#include <chrono>

#include "PeriodicDeadline.hpp"
#include "RadarFaces.hpp"
#include "SimClock.hpp"

namespace radar::app {

void BeamScheduler::start() {
    auto topic = radds::make_topic<types::BeamCommand>(
        participant_, dds_names::TOPIC_BEAM_COMMAND);
    writer_ = radds::make_writer<types::BeamCommand>(
        publisher_, topic, dds_names::PROFILE_BEAM_COMMAND);

    spawn([this] {
        std::array<int64_t, faces::kFaceCount> beam_ids{};
        std::array<search_raster::FaceRasterState, faces::kFaceCount>
            search_states{};
        std::array<search_raster::SectorRasterState, faces::kFaceCount>
            sector_states{};
        std::array<int32_t, faces::kFaceCount> previous_modes{};
        auto next = std::chrono::steady_clock::now();

        while (!stop_.load()) {
            next = advance_periodic_deadline(
                next,
                std::chrono::milliseconds(
                    static_cast<int>(
                        search_raster::kDwellPeriodSec * 1000.0))); // 100 Hz

            for (const auto& face : faces::kDefinitions) {
                const auto i = static_cast<std::size_t>(face.id);
                const int32_t mode = bus_.radar_mode[i].load();
                if (mode != previous_modes[i]) {
                    sector_states[i] = {};
                    previous_modes[i] = mode;
                }

                const auto pointing = mode == 1
                    ? search_raster::advance_sector(
                          bus_.sector_center_deg[i].load(),
                          sector_states[i])
                    : search_raster::advance_face(
                          face.id, search_states[i]);

                types::BeamCommand cmd;
                cmd.scheduler_id = face.id;
                cmd.beam_id = beam_ids[i]++;
                cmd.timestamp = SimClock::stamp();
                cmd.azimuth_deg = pointing.azimuth_deg;
                cmd.elevation_deg = pointing.elevation_deg;
                cmd.dwell_time_us = static_cast<int32_t>(
                    search_raster::kDwellPeriodSec * 1e6);
                cmd.mode     = types::BeamMode::BEAM_MODE_SEARCH;
                cmd.priority = mode == 1 ? 2 : 3;
                writer_.write(cmd);

                bus_.current_beam_az_deg[i].store(pointing.azimuth_deg);
                bus_.current_beam_el_deg[i].store(pointing.elevation_deg);
                bus_.beam_commands.push_overwrite(BeamView{
                    face.id, cmd.beam_id,
                    pointing.azimuth_deg, pointing.elevation_deg,
                    static_cast<int32_t>(cmd.mode), cmd.priority,
                    cmd.timestamp.sim_millis});
            }
            std::this_thread::sleep_until(next);
        }
    });
}

} // namespace radar::app
