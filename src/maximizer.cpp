/*
 *  Copyright © 2017-2023 Wellington Wallace
 *
 *  This file is part of Easy Effects.
 *
 *  Easy Effects is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Easy Effects is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Easy Effects. If not, see <https://www.gnu.org/licenses/>.
 */

#include "maximizer.hpp"

Maximizer::Maximizer(const std::string& tag,
                     const std::string& schema,
                     const std::string& schema_path,
                     PipeManager* pipe_manager)
    : PluginBase(tag, tags::plugin_name::maximizer, tags::plugin_package::zam, schema, schema_path, pipe_manager),
      lv2_wrapper(std::make_unique<lv2::Lv2Wrapper>("urn:zamaudio:ZaMaximX2")) {
  package_installed = lv2_wrapper->found_plugin;

  if (!package_installed) {
    util::debug(log_tag + "urn:zamaudio:ZaMaximX2 is not installed");
  }

  lv2_wrapper->bind_key_double<"thresh", "threshold">(settings);

  lv2_wrapper->bind_key_double<"ceil", "ceiling">(settings);

  lv2_wrapper->bind_key_double<"rel", "release">(settings);

  setup_input_output_gain();
}

Maximizer::~Maximizer() {
  if (connected_to_pw) {
    disconnect_from_pw();
  }

  util::debug(log_tag + name + " destroyed");
}

void Maximizer::setup() {
  if (!lv2_wrapper->found_plugin) {
    return;
  }

  lv2_wrapper->set_n_samples(n_samples);

  if (lv2_wrapper->get_rate() != rate) {
    lv2_wrapper->create_instance(rate);
  }
}

void Maximizer::process(std::span<float>& left_in,
                        std::span<float>& right_in,
                        std::span<float>& left_out,
                        std::span<float>& right_out) {
  if (!lv2_wrapper->found_plugin || !lv2_wrapper->has_instance() || bypass) {
    std::copy(left_in.begin(), left_in.end(), left_out.begin());
    std::copy(right_in.begin(), right_in.end(), right_out.begin());

    return;
  }

  if (input_gain != 1.0F) {
    apply_gain(left_in, right_in, input_gain);
  }

  lv2_wrapper->connect_data_ports(left_in, right_in, left_out, right_out);

  lv2_wrapper->run();

  if (output_gain != 1.0F) {
    apply_gain(left_out, right_out, output_gain);
  }

  /*
    This plugin gives the latency in number of samples
  */

  const auto lv = static_cast<uint>(lv2_wrapper->get_control_port_value("lv2_latency"));

  if (latency_n_frames != lv) {
    latency_n_frames = lv;

    latency_port_value = static_cast<float>(latency_n_frames) / static_cast<float>(rate);

    util::debug(log_tag + name + " latency: " + util::to_string(latency_port_value, "") + " s");

    util::idle_add([=, this]() {
      if (!post_messages || latency.empty()) {
        return;
      }

      latency.emit();
    });

    spa_process_latency_info latency_info{};

    latency_info.ns = static_cast<uint64_t>(latency_port_value * 1000000000.0F);

    std::array<char, 1024U> buffer{};

    spa_pod_builder b{};

    spa_pod_builder_init(&b, buffer.data(), sizeof(buffer));

    const spa_pod* param = spa_process_latency_build(&b, SPA_PARAM_ProcessLatency, &latency_info);

    pw_filter_update_params(filter, nullptr, &param, 1);
  }

  if (post_messages) {
    get_peaks(left_in, right_in, left_out, right_out);

    notification_dt += buffer_duration;

    if (notification_dt >= notification_time_window) {
      // reduction needed as double for levelbar widget ui, so we convert it here

      reduction_port_value = static_cast<double>(lv2_wrapper->get_control_port_value("gr"));

      reduction.emit(reduction_port_value);

      notify();

      notification_dt = 0.0F;
    }
  }
}

auto Maximizer::get_latency_seconds() -> float {
  return latency_port_value;
}
