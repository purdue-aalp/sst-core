#ifndef SST_CORE_WRAPPER_WRAPPER_HH
#define SST_CORE_WRAPPER_WRAPPER_HH

#include "sst/core/sst_config.h"
#include "sst/core/sst_types.h"

namespace SST {
// Forward declarations
class Simulation_impl;
class ConfigGraph;
struct RankInfo;

/**
 * Simple wrapper to run SST simulations from external programs without calling main().
 * Provides a minimal interface for single-threaded, single-process simulations.
 */
class SST_Wrapper {
public:
    /**
     * Constructor - initializes the wrapper for single-threaded execution
     */
    SST_Wrapper();

    /**
     * Destructor - cleans up simulation resources
     */
    ~SST_Wrapper();

    /**
     * Initialize the simulation with a configuration file (Python SDL)
     * @param config_file Path to the SST configuration file
     * @return 0 on success, non-zero on error
     */
    int initialize(const char* config_file);

    /**
     * Run the simulation to completion
     * @return 0 on success, non-zero on error
     */
    int run();

    /**
     * Start the simulation (initialize, setup, prepare for cycle-by-cycle execution)
     * Call this before using cycle()
     * @return 0 on success, non-zero on error
     */
    int start();

    /**
     * Process events at the given cycle (called every cycle by accel-sim)
     * Checks if the next SST event is ready at this cycle and executes it if so.
     * @param current_cycle Current cycle from accel-sim
     * @return true if an event was executed, false otherwise
     */
    bool cycle(uint64_t current_cycle);

    /**
     * Complete the simulation after cycle-by-cycle execution
     * Call this when accel-sim is done
     */
    void complete();

    /**
     * Check if simulation has ended
     * @return true if simulation has ended
     */
    bool isSimulationComplete() const;

    /**
     * Finalize and cleanup
     */
    void finalize();

private:
    Simulation_impl* sim_;
    ConfigGraph*     graph_;
    RankInfo*        world_size_;
    RankInfo*        my_rank_;
    bool             initialized_;
    bool             mpi_initialized_;

    friend class Simulation_impl;
};

} // namespace SST

#endif // SST_CORE_WRAPPER_WRAPPER_HH