//
// ServerlessSimulatorFixed.h
// Fixed version addressing critical bugs and missing functionality
//

#ifndef SERVERLESSSIMULATOR_FIXED_H
#define SERVERLESSSIMULATOR_FIXED_H

#include <vector>
#include <array>
#include <string>
#include <map>

#include "FunctionInstance.h"
#include "SimProcess.h"
#include "AutoScalingAlgorithm.h"

class ServerlessSimulator {
public:
    struct Stats {
        long total_req{0}, total_cold{0}, total_warm{0}, total_reject{0};
        long total_init_free{0}, total_init_reserved{0}, total_queued{0};
        double sum_lifespan{0.0};
        long num_terminated{0};
    };

    struct Hist {
        std::vector<double> times;
        std::vector<int> server_count, running_count, idle_count;
        std::vector<int> init_free_count, init_reserved_count, queued_job_count;
        std::vector<int> req_cold_idx, req_warm_idx, req_rej_idx;
        std::vector<int> req_init_free_idx, req_init_reserved_idx, req_queued_idx;
    };

    // Constructor
    ServerlessSimulator(ExpSimProcess arrival,
                        SimProcess& cold_start,
                        SimProcess& cold_service,
                        SimProcess& warm_service,
                        SimProcess& expiration,
                        int max_concurrency,
                        AutoScalingAlgorithm algo);

    // Initialization
    void initialize_system(double t0, int running_cnt, int idle_cnt,
                          int init_free_cnt, int init_reserved_cnt);

    // Reset for new simulation run
    void reset_trace();

    // Main simulation
    void run(bool progress = false, bool debug = false);

    // Statistics getters
    const Stats& stats() const { return stats_; }
    const Hist& hist() const { return hist_; }

    // FIXED: Corrected method name (was print_sunmmary)
    void print_summary() const;

    // NEW: Result analysis methods (matching Python)
    double get_trace_end() const;
    void calculate_time_lengths() const;  // Made const for use in const methods
    double get_average_server_count() const;
    double get_average_server_running_count() const;
    double get_average_server_idle_count() const;
    double get_average_server_init_free_count() const;
    double get_average_server_init_reserved_count() const;
    double get_average_server_queued_jobs_count() const;
    double get_average_lifespan() const;
    double get_cold_start_prob() const;

    // NEW: Result dictionary-like structure
    struct ResultDict {
        long reqs_cold;
        long reqs_total;
        long reqs_init_free;
        long reqs_init_reserved;
        long reqs_warm;
        long reqs_queued;
        long reqs_reject;
        double prob_cold;
        double prob_reject;
        double lifespan_avg;
        double inst_count_avg;
        double inst_running_count_avg;
        double inst_idle_count_avg;
        double inst_init_free_count_avg;
        double inst_init_reserved_count_avg;
        double inst_queued_jobs_count_avg;
    };
    ResultDict get_result_dict();

    // NEW: Save results to CSV
    void save_results_to_csv(const std::string& log_dir) const;

private:
    // Utility methods
    bool has_server() const { return !servers_.empty(); }
    bool is_warm_available() const { return idle_count_ > 0; }
    bool is_init_free_available() const { return init_free_count_ > 0; }

    // Arrival handlers
    void warm_start_arrival(double t, int theta);
    void init_free_arrival(double t, int theta);
    void cold_start_arrival(double t, int theta);

    // Inventory management
    void start_init_free_servers(double t, int theta, const char* why);

    // Selectors
    int pick_newest_idle() const;
    int pick_newest_init_free() const;

    // State management
    void update_hist_arrays(double t);
    void update_state_vector();

    // NEW: Cost calculation wrapper
    double calculate_cost();

    // Processes
    ExpSimProcess arrival_;
    SimProcess& cold_start_;
    SimProcess& cold_service_;
    SimProcess& warm_service_;
    SimProcess& expiration_;

    // Autoscaler
    AutoScalingAlgorithm algo_;

    // Capacity and instance pool
    int max_conc_{0};
    std::vector<FunctionInstance> servers_;

    // NEW: Terminated servers for lifespan tracking
    std::vector<FunctionInstance> prev_servers_;

    // Counters
    int server_count_{0}, running_count_{0}, idle_count_{0};
    int init_free_count_{0}, init_reserved_count_{0};
    int init_free_booked_{0}, queued_jobs_count_{0};

    // NEW: Job rejection tracking
    bool job_rejected_{false};
    int missed_update_{0};

    // History and statistics
    Hist hist_;
    Stats stats_;

    // NEW: Time lengths for weighted statistics (mutable for lazy calculation in const methods)
    mutable std::vector<double> time_lengths_;

    // State vector for autoscaler
    std::array<int, 5> state_{};

    // Time tracking
    double t_{0.0};
    double next_arrival_{0.0};
};

#endif // SERVERLESSSIMULATOR_FIXED_H
