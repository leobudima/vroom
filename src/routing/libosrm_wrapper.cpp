/*

This file is part of VROOM.

Copyright (c) 2015-2025, Julien Coupey.
All rights reserved (see LICENSE).

*/

#include <cstdint>

#include "osrm/coordinate.hpp"
#include "osrm/json_container.hpp"
#include "osrm/route_parameters.hpp"
#include "osrm/status.hpp"
#include "osrm/table_parameters.hpp"

#include "routing/libosrm_wrapper.h"
#include "utils/helpers.h"

namespace vroom {
namespace routing {

osrm::EngineConfig LibosrmWrapper::get_config(const std::string& profile) {
  osrm::EngineConfig config;

  // Only update non-default values.
  config.max_alternatives = 1;
  config.dataset_name = profile;

  return config;
}

LibosrmWrapper::LibosrmWrapper(const std::string& profile)
  : Wrapper(profile), _config(get_config(profile)), _osrm(_config) {
}

void throw_error(osrm::json::Object& result,
                 const std::vector<Location>& locs) {
  const std::string code =
    std::get<osrm::json::String>(result.values["code"]).value;
  const std::string message =
    std::get<osrm::json::String>(result.values["message"]).value;

  const std::string snapping_error_base =
    "Could not find a matching segment for coordinate ";
  if (code == "NoSegment" && message.starts_with(snapping_error_base)) {
    auto error_loc =
      std::stoul(message.substr(snapping_error_base.size(),
                                message.size() - snapping_error_base.size()));
    auto coordinates =
      std::format("[{},{}]", locs[error_loc].lon(), locs[error_loc].lat());
    throw RoutingException("Could not find route near location " + coordinates);
  }

  // Other error in response.
  throw RoutingException("libOSRM: " + code + ": " + message);
}

Matrices LibosrmWrapper::get_matrices(const std::vector<Location>& locs) const {
  osrm::TableParameters params;
  params.annotations = osrm::engine::api::TableParameters::AnnotationsType::All;

  params.coordinates.reserve(locs.size());
  params.radiuses.reserve(locs.size());
  for (auto const& location : locs) {
    params.coordinates
      .emplace_back(osrm::util::FloatLongitude({location.lon()}),
                    osrm::util::FloatLatitude({location.lat()}));
    params.radiuses.emplace_back(DEFAULT_LIBOSRM_SNAPPING_RADIUS);
  }

  osrm::json::Object result;
  osrm::Status status = _osrm.Table(params, result);

  if (status == osrm::Status::Error) {
    throw_error(result, locs);
  }

  const auto& durations =
    std::get<osrm::json::Array>(result.values["durations"]);
  const auto& distances =
    std::get<osrm::json::Array>(result.values["distances"]);

  // Expected matrix size.
  std::size_t m_size = locs.size();
  assert(durations.values.size() == m_size);
  assert(distances.values.size() == m_size);

  // Build matrix while checking for unfound routes to avoid
  // unexpected behavior (OSRM raises 'null').
  Matrices m(m_size);

  std::vector<unsigned> nb_unfound_from_loc(m_size, 0);
  std::vector<unsigned> nb_unfound_to_loc(m_size, 0);

  std::string reason;
  for (std::size_t i = 0; i < m_size; ++i) {
    const auto& duration_line =
      std::get<osrm::json::Array>(durations.values.at(i));
    const auto& distance_line =
      std::get<osrm::json::Array>(distances.values.at(i));
    assert(duration_line.values.size() == m_size);
    assert(distance_line.values.size() == m_size);

    for (std::size_t j = 0; j < m_size; ++j) {
      const auto& duration_el = duration_line.values.at(j);
      const auto& distance_el = distance_line.values.at(j);
      if (std::holds_alternative<osrm::json::Null>(duration_el) ||
          std::holds_alternative<osrm::json::Null>(distance_el)) {
        // No route found between i and j. Just storing info as we
        // don't know yet which location is responsible between i
        // and j.
        ++nb_unfound_from_loc[i];
        ++nb_unfound_to_loc[j];
      } else {
        m.durations[i][j] = utils::round<UserDuration>(
          std::get<osrm::json::Number>(duration_el).value);
        m.distances[i][j] = utils::round<UserDistance>(
          std::get<osrm::json::Number>(distance_el).value);
      }
    }
  }

  check_unfound(locs, nb_unfound_from_loc, nb_unfound_to_loc);

  return m;
}

osrm::json::Object LibosrmWrapper::get_route_with_coordinates(
  const std::vector<Location>& locs) const {
  std::vector<osrm::util::Coordinate> coords;
  coords.reserve(locs.size());

  for (const auto& loc : locs) {
    coords.emplace_back(osrm::util::FloatLongitude({loc.lon()}),
                        osrm::util::FloatLatitude({loc.lat()}));
  }

  // Default options for routing.
  osrm::RouteParameters
    params(false, // steps
           false, // alternatives
           osrm::RouteParameters::GeometriesType::Polyline,
           osrm::RouteParameters::OverviewType::Full,
           false, // continue_straight,
           std::move(coords),
           std::vector<std::optional<osrm::engine::Hint>>(),
           std::vector<std::optional<double>>(coords.size(),
                                              DEFAULT_LIBOSRM_SNAPPING_RADIUS));

  osrm::json::Object result;
  osrm::Status status = _osrm.Route(params, result);

  if (status == osrm::Status::Error) {
    throw_error(result, locs);
  }

  auto& result_routes = std::get<osrm::json::Array>(result.values["routes"]);
  return std::move(std::get<osrm::json::Object>(result_routes.values.at(0)));
}

void LibosrmWrapper::update_sparse_matrix(
  const std::vector<Location>& route_locs,
  Matrices& m,
  std::mutex& matrix_m,
  std::string& vehicle_geometry) const {
  auto json_route = get_route_with_coordinates(route_locs);

  auto& legs = std::get<osrm::json::Array>(json_route.values["legs"]);
  assert(legs.values.size() == route_locs.size() - 1);

  for (std::size_t i = 0; i < legs.values.size(); ++i) {
    auto& leg = std::get<osrm::json::Object>(legs.values.at(i));

    std::scoped_lock<std::mutex> lock(matrix_m);
    m.durations[route_locs[i].index()][route_locs[i + 1].index()] =
      utils::round<UserDuration>(
        std::get<osrm::json::Number>(leg.values["duration"]).value);
    m.distances[route_locs[i].index()][route_locs[i + 1].index()] =
      utils::round<UserDistance>(
        std::get<osrm::json::Number>(leg.values["distance"]).value);
  }

  vehicle_geometry = std::move(
    std::get<osrm::json::String>(json_route.values["geometry"]).value);
};

void LibosrmWrapper::add_geometry(Route& route) const {
  std::vector<Location> locs;
  locs.reserve(route.steps.size());

  // Ordering locations for the given steps, excluding
  // breaks.
  for (const auto& step : route.steps) {
    if (step.step_type != STEP_TYPE::BREAK) {
      assert(step.location.has_value());
      locs.emplace_back(step.location.value());
    }
  }

  auto json_route = get_route_with_coordinates(locs);

  // Total distance and route geometry.
  route.geometry = std::move(
    std::get<osrm::json::String>(json_route.values["geometry"]).value);
}

} // namespace routing
} // namespace vroom
