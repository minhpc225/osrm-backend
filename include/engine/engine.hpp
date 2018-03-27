#ifndef ENGINE_HPP
#define ENGINE_HPP

#include "engine/api/match_parameters.hpp"
#include "engine/api/nearest_parameters.hpp"
#include "engine/api/route_parameters.hpp"
#include "engine/api/table_parameters.hpp"
#include "engine/api/tile_parameters.hpp"
#include "engine/api/trip_parameters.hpp"
#include "engine/data_watchdog.hpp"
#include "engine/datafacade/contiguous_block_allocator.hpp"
#include "engine/datafacade_provider.hpp"
#include "engine/engine_config.hpp"
#include "engine/plugins/match.hpp"
#include "engine/plugins/nearest.hpp"
#include "engine/plugins/table.hpp"
#include "engine/plugins/tile.hpp"
#include "engine/plugins/trip.hpp"
#include "engine/plugins/viaroute.hpp"
#include "engine/routing_algorithms.hpp"
#include "engine/status.hpp"

#include "storage/serialization.hpp"

#include "util/exception.hpp"
#include "util/exception_utils.hpp"
#include "util/fingerprint.hpp"
#include "util/json_container.hpp"

#include <memory>
#include <string>

namespace osrm
{
namespace engine
{

class EngineInterface
{
  public:
    virtual ~EngineInterface() = default;
    virtual Status Route(const api::RouteParameters &parameters,
                         util::json::Object &result) const = 0;
    virtual Status Table(const api::TableParameters &parameters,
                         util::json::Object &result) const = 0;
    virtual Status Nearest(const api::NearestParameters &parameters,
                           util::json::Object &result) const = 0;
    virtual Status Trip(const api::TripParameters &parameters,
                        util::json::Object &result) const = 0;
    virtual Status Match(const api::MatchParameters &parameters,
                         util::json::Object &result) const = 0;
    virtual Status Tile(const api::TileParameters &parameters, std::string &result) const = 0;
};

template <typename Algorithm> class Engine final : public EngineInterface
{
  public:
    explicit Engine(const EngineConfig &config)
        : route_plugin(config.max_locations_viaroute, config.max_alternatives),            //
          table_plugin(config.max_locations_distance_table),                               //
          nearest_plugin(config.max_results_nearest),                                      //
          trip_plugin(config.max_locations_trip),                                          //
          match_plugin(config.max_locations_map_matching, config.max_radius_map_matching), //
          tile_plugin()                                                                    //

    {
        if (config.use_shared_memory)
        {
            util::Log(logDEBUG) << "Using shared memory with algorithm "
                                << routing_algorithms::name<Algorithm>();
            facade_provider = std::make_unique<WatchingProvider<Algorithm>>();
        }
        else if (!config.memory_file.empty())
        {
            util::Log(logDEBUG) << "Using memory mapped filed at " << config.memory_file
                                << " with algorithm " << routing_algorithms::name<Algorithm>();
            facade_provider = std::make_unique<ExternalProvider<Algorithm>>(config.storage_config,
                                                                            config.memory_file);
        }
        else
        {
            util::Log(logDEBUG) << "Using internal memory with algorithm "
                                << routing_algorithms::name<Algorithm>();
            facade_provider = std::make_unique<ImmutableProvider<Algorithm>>(config.storage_config);
        }
    }

    Engine(Engine &&) noexcept = delete;
    Engine &operator=(Engine &&) noexcept = delete;

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;
    virtual ~Engine() = default;

    Status Route(const api::RouteParameters &params,
                 util::json::Object &result) const override final
    {
        return route_plugin.HandleRequest(GetAlgorithms(params), params, result);
    }

    Status Table(const api::TableParameters &params,
                 util::json::Object &result) const override final
    {
        return table_plugin.HandleRequest(GetAlgorithms(params), params, result);
    }

    Status Nearest(const api::NearestParameters &params,
                   util::json::Object &result) const override final
    {
        return nearest_plugin.HandleRequest(GetAlgorithms(params), params, result);
    }

    Status Trip(const api::TripParameters &params, util::json::Object &result) const override final
    {
        return trip_plugin.HandleRequest(GetAlgorithms(params), params, result);
    }

    Status Match(const api::MatchParameters &params,
                 util::json::Object &result) const override final
    {
        return match_plugin.HandleRequest(GetAlgorithms(params), params, result);
    }

    Status Tile(const api::TileParameters &params, std::string &result) const override final
    {
        return tile_plugin.HandleRequest(GetAlgorithms(params), params, result);
    }

    static bool CheckCompatibility(const EngineConfig &config);

  private:
    template <typename ParametersT> auto GetAlgorithms(const ParametersT &params) const
    {
        return RoutingAlgorithms<Algorithm>{heaps, facade_provider->Get(params)};
    }
    std::unique_ptr<DataFacadeProvider<Algorithm>> facade_provider;
    mutable SearchEngineData<Algorithm> heaps;

    const plugins::ViaRoutePlugin route_plugin;
    const plugins::TablePlugin table_plugin;
    const plugins::NearestPlugin nearest_plugin;
    const plugins::TripPlugin trip_plugin;
    const plugins::MatchPlugin match_plugin;
    const plugins::TilePlugin tile_plugin;
};

template <>
bool Engine<routing_algorithms::ch::Algorithm>::CheckCompatibility(const EngineConfig &config)
{
    if (config.use_shared_memory)
    {
        storage::SharedMonitor<storage::SharedDataTimestamp> barrier;
        using mutex_type = typename decltype(barrier)::mutex_type;
        boost::interprocess::scoped_lock<mutex_type> current_region_lock(barrier.get_mutex());

        auto mem = storage::makeSharedMemory(barrier.data().region);
        storage::DataLayout layout;
        storage::io::BufferReader reader(reinterpret_cast<const char *>(mem->Ptr()), mem->Size());
        storage::serialization::read(reader, layout);

        std::vector<std::string> metric_prefixes;
        layout.List("/ch/metrics/", std::back_inserter(metric_prefixes));

        bool has_graph = false;
        for (const auto &metric_prefix : metric_prefixes)
        {
            has_graph |= layout.HasBlock(metric_prefix + "/contracted_graph/node_array") &&
                         layout.HasBlock(metric_prefix + "/contracted_graph/edge_array");
        }
        return has_graph;
    }
    else
    {
        return boost::filesystem::exists(config.storage_config.GetPath(".osrm.hsgr"));
    }
}

template <>
bool Engine<routing_algorithms::mld::Algorithm>::CheckCompatibility(const EngineConfig &config)
{
    if (config.use_shared_memory)
    {
        storage::SharedMonitor<storage::SharedDataTimestamp> barrier;
        using mutex_type = typename decltype(barrier)::mutex_type;
        boost::interprocess::scoped_lock<mutex_type> current_region_lock(barrier.get_mutex());

        auto mem = storage::makeSharedMemory(barrier.data().region);
        storage::DataLayout layout;
        storage::io::BufferReader reader(reinterpret_cast<const char *>(mem->Ptr()), mem->Size());
        storage::serialization::read(reader, layout);

        std::vector<std::string> metric_prefixes;
        layout.List("/mld/metrics/", std::back_inserter(metric_prefixes));

        bool has_cells = false;
        for (const auto &metric_prefix : metric_prefixes)
        {
            has_cells |= layout.HasBlock(metric_prefix + "/exclude/0/durations") &&
                         layout.HasBlock(metric_prefix + "/exclude/0/weights");
        }

        // checks that all the needed memory blocks are populated
        // "/mld/cellstorage/source_boundary" and "/mld/cellstorage/destination_boundary"
        // are not checked, because in situations where there are so few nodes in the graph that
        // they all fit into one cell, they can be empty.
        bool has_data = has_cells && layout.HasBlock("/mld/multilevelpartition/level_data") &&
                        layout.HasBlock("/mld/multilevelpartition/partition") &&
                        layout.HasBlock("/mld/multilevelpartition/cell_to_children") &&
                        layout.HasBlock("/mld/cellstorage/cells") &&
                        layout.HasBlock("/mld/cellstorage/level_to_cell_offset") &&
                        layout.HasBlock("/mld/multilevelgraph/node_array") &&
                        layout.HasBlock("/mld/multilevelgraph/edge_array") &&
                        layout.HasBlock("/mld/multilevelgraph/node_to_edge_offset");
        return has_data;
    }
    else
    {
        return boost::filesystem::exists(config.storage_config.GetPath(".osrm.partition")) &&
               boost::filesystem::exists(config.storage_config.GetPath(".osrm.cells")) &&
               boost::filesystem::exists(config.storage_config.GetPath(".osrm.mldgr")) &&
               boost::filesystem::exists(config.storage_config.GetPath(".osrm.cell_metrics"));
    }
}
}
}

#endif // OSRM_IMPL_HPP
