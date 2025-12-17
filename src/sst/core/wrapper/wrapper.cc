#include "wrapper.hh"
#include "sst/core/simulation_impl.h"
#include "sst/core/config.h"
#include "sst/core/configGraph.h"
#include "sst/core/rankInfo.h"
#include "sst/core/factory.h"
#include "sst/core/model/sstmodel.h"
#include "sst/core/timeLord.h"
#include "sst/core/output.h"
#include "sst/core/eli/elementinfo.h"
#include "sst/core/unitAlgebra.h"
#include "sst/core/statapi/statengine.h"
#include "sst/core/checkpointAction.h"
#include "sst/core/mempool.h"
#include "sst/core/mempoolAccessor.h"
#include "sst/core/activity.h"
#include "sst/core/timeVortex.h"

#include <iostream>
#include <mpi.h>

namespace SST {

SST_Wrapper::SST_Wrapper()
    : sim_(nullptr)
    , graph_(nullptr)
    , world_size_(new RankInfo(1, 1))  // Single process, single thread
    , my_rank_(new RankInfo(0, 0))     // Rank 0, thread 0
    , initialized_(false)
    , mpi_initialized_(false)
{
    // Initialize MPI if not already initialized
    int mpi_init_flag = 0;
    MPI_Initialized(&mpi_init_flag);
    if (!mpi_init_flag) {
        int argc = 0;
        char** argv = nullptr;
        MPI_Init(&argc, &argv);
        mpi_initialized_ = true;
    }

    // Initialize config for single-threaded, single-rank execution
    Simulation_impl::config.initialize(1, true);
}

SST_Wrapper::~SST_Wrapper() {
    finalize();
    delete world_size_;
    delete my_rank_;
}

int SST_Wrapper::initialize(const char* config_file) {
    if (initialized_) {
        std::cerr << "SST_Wrapper: Already initialized!" << std::endl;
        return -1;
    }

    Config& cfg = Simulation_impl::config;

    // Set configuration file
    cfg.configFile_ = config_file;

    // Force single-threaded execution
    cfg.num_threads_ = 1;

    // Initialize UnitAlgebra (required for time conversions)
    Units::registerBaseUnit("s");
    Units::registerBaseUnit("B");
    Units::registerBaseUnit("b");
    Units::registerBaseUnit("events");
    Units::registerCompoundUnit("Hz", "1/s");
    Units::registerCompoundUnit("hz", "1/s");
    Units::registerCompoundUnit("Bps", "B/s");
    Units::registerCompoundUnit("bps", "b/s");
    Units::registerCompoundUnit("event", "events");

    // Create Factory
    Factory::createFactory(cfg.getLibPath());

    // Load configuration graph from SDL file
    try {
        // Get the model generator for Python files
        auto modelGen = Factory::createModelDescription(
            "sst.model.python",
            config_file,
            0,  // verbose = 0
            cfg,
            0.0  // start time
        );

        graph_ = modelGen->createConfigGraph();
        delete modelGen;

    } catch (std::exception& e) {
        std::cerr << "SST_Wrapper: Error loading config: " << e.what() << std::endl;
        return -1;
    }

    // Initialize TimeLord with default timebase
    Simulation_impl::getTimeLord()->init(cfg.timeBase());

    // Initialize output system
    Output::setFileName("sst_output");
    Output::setWorldSize(1, 1, 0);
    auto g_output = Output::setDefaultObject("", 0, 0, Output::STDOUT);

    // Setup graph
    graph_->postCreationCleanup();
    if (graph_->checkForStructuralErrors()) {
        std::cerr << "SST_Wrapper: Structural errors in config graph!" << std::endl;
        return -1;
    }

    // Use single partitioner (assigns all components to rank 0)
    cfg.partitioner_ = "sst.single";
    auto* partitioner = Factory::getFactory()->CreatePartitioner(
        cfg.partitioner_, *world_size_, *my_rank_, 0);
    partitioner->performPartition(graph_);
    delete partitioner;

    // Initialize global simulation objects
    Simulation_impl::factory = Factory::getFactory();
    Simulation_impl::sim_output = g_output;
    Simulation_impl::resizeBarriers(1);
    CheckpointAction::barrier.resize(1);

#ifdef USE_MEMPOOL
    Core::MemPoolAccessor::initializeGlobalData(1, cfg.cache_align_mempools());
    Core::MemPoolAccessor::initializeLocalData(0);
#endif

    // Create the simulation instance
    sim_ = Simulation_impl::createSimulation(*my_rank_, *world_size_, false, 0, 0);

    // Process graph and wire up components
    SimTime_t min_part = 0xffffffffffffffffl;
    sim_->processGraphInfo(*graph_, *my_rank_, min_part);
    sim_->prepareLinks(*graph_, *my_rank_, min_part);
    sim_->performWireUp(*graph_, *my_rank_, min_part);
    sim_->exchangeLinkInfo();

    // Initialize statistics
    auto* stats_config = graph_->getStatsConfig();
    Statistics::StatisticProcessingEngine::static_setup(stats_config);
    sim_->initializeStatisticEngine(stats_config);

    initialized_ = true;
    return 0;
}

int SST_Wrapper::run() {
    if (!initialized_ || !sim_) {
        std::cerr << "SST_Wrapper: Not initialized! Call initialize() first." << std::endl;
        return -1;
    }

    try {
        // Run simulation phases
        sim_->initialize();  // Component init()
        sim_->setup();       // Component setup()

        Statistics::StatisticProcessingEngine::stat_outputs_simulation_start();
        sim_->prepare_for_run();

        sim_->run();         // Main event loop

        sim_->adjustTimeAtSimEnd();
        sim_->complete();    // Component complete()
        sim_->finish();      // Component finish()

        Statistics::StatisticProcessingEngine::stat_outputs_simulation_end();

        std::cout << "Simulation complete, simulated time: "
                  << sim_->getEndSimTime().toStringBestSI() << std::endl;

    } catch (std::exception& e) {
        std::cerr << "SST_Wrapper: Error during simulation: " << e.what() << std::endl;
        return -1;
    }

    return 0;
}

int SST_Wrapper::start() {
    if (!initialized_ || !sim_) {
        std::cerr << "SST_Wrapper: Not initialized! Call initialize() first." << std::endl;
        return -1;
    }

    try {
        // Run simulation initialization phases
        sim_->initialize();  // Component init()
        sim_->setup();       // Component setup()

        Statistics::StatisticProcessingEngine::stat_outputs_simulation_start();
        sim_->prepare_for_run();

        std::cout << "Simulation started and ready for cycle-by-cycle execution" << std::endl;

    } catch (std::exception& e) {
        std::cerr << "SST_Wrapper: Error during simulation start: " << e.what() << std::endl;
        return -1;
    }

    return 0;
}

bool SST_Wrapper::cycle(uint64_t current_cycle) {
    if (!initialized_ || !sim_) {
        return false;
    }

    // Check if simulation has ended
    if (sim_->endSim) {
        return false;
    }

    // Get the next event's delivery time
    SimTime_t next_event_time = sim_->getNextActivityTime();

    // If next event is at or before current cycle, execute it
    if (next_event_time <= current_cycle) {
        // Pop and execute the event
        Activity* activity = sim_->timeVortex->pop();

        // Update simulation time
        sim_->currentSimCycle = activity->getDeliveryTime();
        sim_->currentPriority = activity->getPriority();

        // Execute the event
        activity->execute();

        return true;  // Event was executed
    }

    return false;  // No event ready at this cycle
}

void SST_Wrapper::complete() {
    if (!initialized_ || !sim_) {
        return;
    }

    try {
        sim_->adjustTimeAtSimEnd();
        sim_->complete();    // Component complete()
        sim_->finish();      // Component finish()

        Statistics::StatisticProcessingEngine::stat_outputs_simulation_end();

        std::cout << "Simulation complete, simulated time: "
                  << sim_->getEndSimTime().toStringBestSI() << std::endl;

    } catch (std::exception& e) {
        std::cerr << "SST_Wrapper: Error during simulation completion: " << e.what() << std::endl;
    }
}

bool SST_Wrapper::isSimulationComplete() const {
    if (!sim_) {
        return true;
    }
    return sim_->endSim;
}

void SST_Wrapper::finalize() {
    if (sim_) {
        delete sim_;
        sim_ = nullptr;
    }

    if (graph_) {
        delete graph_;
        graph_ = nullptr;
    }

    if (initialized_) {
        Simulation_impl::shutdown();
        initialized_ = false;
    }

    // Finalize MPI if we initialized it
    if (mpi_initialized_) {
        MPI_Finalize();
        mpi_initialized_ = false;
    }
}

} // namespace SST
