//
// ServerlessSimulatorFixed.cpp
// Fixed version addressing critical bugs identified in analysis
//

#include "SimProcess.h"
#include "FunctionInstance.h"
#include "ServerlessSimulatorFixed.h"

#include <algorithm>
#include <limits>
#include <iomanip>
#include <numeric>
#include <fstream>
#include <sstream>
#include <cmath>

ServerlessSimulator::ServerlessSimulator(ExpSimProcess arrival,
                                         SimProcess& cold_start,
                                         SimProcess& cold_service,
                                         SimProcess& warm_service,
                                         SimProcess& expiration,
                                         int max_concurrency,
                                         AutoScalingAlgorithm algo)
    : arrival_(std::move(arrival)),
      cold_start_(cold_start),
      cold_service_(cold_service),
      warm_service_(warm_service),
      expiration_(expiration),
      algo_(std::move(algo)),
      max_conc_(max_concurrency) {
    // First arrival
    next_arrival_ = t_ + arrival_.sample();
}

void ServerlessSimulator::reset_trace() {
    // Clear all state
    servers_.clear();
    prev_servers_.clear();

    // Reset counters
    server_count_ = 0;
    running_count_ = 0;
    idle_count_ = 0;
    init_free_count_ = 0;
    init_reserved_count_ = 0;
    init_free_booked_ = 0;
    queued_jobs_count_ = 0;

    // Reset flags
    job_rejected_ = false;
    missed_update_ = 0;

    // Clear history
    hist_.times.clear();
    hist_.server_count.clear();
    hist_.running_count.clear();
    hist_.idle_count.clear();
    hist_.init_free_count.clear();
    hist_.init_reserved_count.clear();
    hist_.queued_job_count.clear();
    hist_.req_cold_idx.clear();
    hist_.req_warm_idx.clear();
    hist_.req_rej_idx.clear();
    hist_.req_init_free_idx.clear();
    hist_.req_init_reserved_idx.clear();
    hist_.req_queued_idx.clear();

    time_lengths_.clear();

    // Reset statistics
    stats_ = Stats{};

    // Reset time
    t_ = 0.0;
    next_arrival_ = t_ + arrival_.sample();
}

void ServerlessSimulator::initialize_system(double t0, int running_cnt, int idle_cnt,
                                           int init_free_cnt, int init_reserved_cnt) {
    t_ = t0;
    servers_.reserve(running_cnt + idle_cnt + init_free_cnt + init_reserved_cnt);

    // Create idle instances
    for (int i = 0; i < idle_cnt; ++i) {
        FunctionInstance f(t_, cold_start_, cold_service_, warm_service_, expiration_);
        f.make_Init_Free(t0);
        f.make_transition();
        servers_.push_back(std::move(f));
        ++idle_count_;
        ++server_count_;
    }

    // Create running instances
    for (int i = 0; i < running_cnt; ++i) {
        FunctionInstance f(t_, cold_start_, cold_service_, warm_service_, expiration_);
        f.make_Init_Free(t0);
        f.make_transition();
        f.arrival_transition(t_);
        servers_.push_back(std::move(f));
        ++running_count_;
        ++server_count_;
    }

    // Create init free instances
    for (int i = 0; i < init_free_cnt; ++i) {
        FunctionInstance f(t_, cold_start_, cold_service_, warm_service_, expiration_);
        f.make_Init_Free(t0);
        servers_.push_back(std::move(f));
        ++init_free_count_;
        ++server_count_;
    }

    // Create init reserved instances
    for (int i = 0; i < init_reserved_cnt; ++i) {
        FunctionInstance f(t_, cold_start_, cold_service_, warm_service_, expiration_);
        f.make_Init_Reserved(t0);
        servers_.push_back(std::move(f));
        ++init_reserved_count_;
        ++server_count_;
    }
}

int ServerlessSimulator::pick_newest_idle() const {
    // FIXED: Explicitly find the instance with maximum creation time
    // instead of assuming ordering
    int idx = -1;
    double best_ct = -1e300;
    for (int i = 0; i < (int)servers_.size(); ++i) {
        if (servers_[i].is_idle_on() && servers_[i].creation_time() > best_ct) {
            best_ct = servers_[i].creation_time();
            idx = i;
        }
    }
    return idx;
}

int ServerlessSimulator::pick_newest_init_free() const {
    // FIXED: Explicitly find the instance with maximum creation time
    int idx = -1;
    double best_ct = -1e300;
    for (int i = 0; i < (int)servers_.size(); ++i) {
        const auto& s = servers_[i];
        if (s.is_init_free() && !s.is_reserved() && s.creation_time() > best_ct) {
            best_ct = s.creation_time();
            idx = i;
        }
    }
    return idx;
}

void ServerlessSimulator::update_hist_arrays(double t) {
    hist_.server_count.push_back(server_count_);
    hist_.running_count.push_back(running_count_);
    hist_.idle_count.push_back(idle_count_);
    hist_.init_reserved_count.push_back(init_reserved_count_);
    hist_.init_free_count.push_back(init_free_count_);
    hist_.queued_job_count.push_back(queued_jobs_count_);
}

void ServerlessSimulator::update_state_vector() {
    state_.fill(0);
    state_[0] = init_free_count_ + init_reserved_count_ + init_free_booked_;
    state_[1] = init_reserved_count_ + init_free_booked_;
    state_[2] = running_count_;
    state_[3] = idle_count_;
    state_[4] = max_conc_ - (init_free_count_ + init_free_booked_ + init_reserved_count_ +
                             running_count_ + idle_count_);
}

double ServerlessSimulator::calculate_cost() {
    // FIXED: Wrapper for cost calculation with job rejection flag reset
    double cost = algo_.compute_cost(state_);
    algo_.set_has_rejected_job(job_rejected_);
    job_rejected_ = false;
    return cost;
}

void ServerlessSimulator::start_init_free_servers(double t, int theta, const char* why) {
    if (theta < 0) return;

    int current_conc = running_count_ + init_reserved_count_ + init_free_booked_;
    int cold_servers = max_conc_ - (init_free_count_ + init_free_booked_ + init_reserved_count_ +
                                     running_count_ + idle_count_);
    int pi_theta = std::max(0, theta - init_free_count_);
    int to_start = (current_conc + pi_theta > max_conc_) ? (max_conc_ - current_conc) : pi_theta;
    to_start = std::min(to_start, cold_servers);

    for (int i = 0; i < to_start; ++i) {
        FunctionInstance f(t, cold_start_, cold_service_, warm_service_, expiration_);
        f.make_Init_Free(t);
        servers_.push_back(std::move(f));
        ++init_free_count_;
        ++server_count_;
        ++stats_.total_init_free;
        if (!hist_.times.empty())
            hist_.req_init_free_idx.push_back((int)hist_.times.size() - 1);
    }
}

void ServerlessSimulator::warm_start_arrival(double t, int theta) {
    ++stats_.total_req;
    int current_conc = running_count_ + init_reserved_count_ + init_free_booked_;

    if (current_conc >= max_conc_) {
        ++stats_.total_reject;
        if (!hist_.times.empty())
            hist_.req_rej_idx.push_back((int)hist_.times.size() - 1);
        job_rejected_ = true;
        return;
    }

    int idx = pick_newest_idle();
    if (idx < 0) {
        cold_start_arrival(t, theta);
        return;
    }

    bool was_idle = servers_[idx].is_idle_on();
    servers_[idx].arrival_transition(t);
    ++stats_.total_warm;
    if (was_idle) {
        --idle_count_;
        ++running_count_;
    }
    if (!hist_.times.empty())
        hist_.req_warm_idx.push_back((int)hist_.times.size() - 1);

    // FIXED: Maintain inventory - use theta parameter as in Python
    auto step = algo_.get_theta_step();
    int theta_idle = step[1];
    if (idle_count_ < theta_idle) {
        int to_start = theta - idle_count_;  // FIXED: Use theta, not step[0]
        if (to_start < 0) {
            ++missed_update_;
        }
        if (to_start > 0) {
            start_init_free_servers(t, to_start, "Warm start");
        }
    }
}

void ServerlessSimulator::init_free_arrival(double t, int theta) {
    ++stats_.total_req;
    ++stats_.total_queued;
    ++queued_jobs_count_;

    int current_conc = running_count_ + init_reserved_count_ + init_free_booked_;
    if (current_conc >= max_conc_) {
        ++stats_.total_reject;
        if (!hist_.times.empty())
            hist_.req_rej_idx.push_back((int)hist_.times.size() - 1);
        job_rejected_ = true;
        return;
    }

    int idx = pick_newest_init_free();
    if (idx < 0) {
        ++stats_.total_reject;
        if (!hist_.times.empty())
            hist_.req_rej_idx.push_back((int)hist_.times.size() - 1);
        job_rejected_ = true;
        return;
    }

    servers_[idx].arrival_transition(t);
    --init_free_count_;
    ++init_free_booked_;
    if (!hist_.times.empty())
        hist_.req_queued_idx.push_back((int)hist_.times.size() - 1);

    start_init_free_servers(t, theta, "Init free arrival");
}

void ServerlessSimulator::cold_start_arrival(double t, int theta) {
    ++stats_.total_req;
    int current_conc = running_count_ + init_reserved_count_ + init_free_booked_;
    int cold_servers = max_conc_ - (init_free_count_ + init_free_booked_ + init_reserved_count_ +
                                     running_count_ + idle_count_);

    if (current_conc >= max_conc_ || cold_servers <= 0) {
        ++stats_.total_reject;
        if (!hist_.times.empty())
            hist_.req_rej_idx.push_back((int)hist_.times.size() - 1);
        job_rejected_ = true;
        return;
    }

    ++stats_.total_cold;
    if (!hist_.times.empty()) {
        hist_.req_cold_idx.push_back((int)hist_.times.size() - 1);
        hist_.req_init_reserved_idx.push_back((int)hist_.times.size() - 1);
    }

    FunctionInstance f(t, cold_start_, cold_service_, warm_service_, expiration_);
    f.make_Init_Reserved(t);
    servers_.push_back(std::move(f));
    ++init_reserved_count_;
    ++server_count_;
    ++stats_.total_init_reserved;

    start_init_free_servers(t_, theta, "Cold start");
}

void ServerlessSimulator::run(bool progress, bool debug) {

    while (algo_.running_condition()) {
        // Find next instance event
        double min_inst_dt = std::numeric_limits<double>::infinity();
        int min_idx = -1;
        for (int i = 0; i < (int)servers_.size(); ++i) {
            double dt = servers_[i].get_next_transition_time(t_);
            if (dt < min_inst_dt) {
                min_inst_dt = dt;
                min_idx = i;
            }
        }
        double next_event_t = t_ + min_inst_dt;

        // Is arrival earlier?
        if (!has_server() || next_arrival_ < next_event_t) {
            t_ = next_arrival_;
            hist_.times.push_back(t_);
            update_hist_arrays(t_);

            auto step = algo_.get_theta_step();
            int theta = step[0];

            if (is_warm_available()) {
                warm_start_arrival(t_, theta);
            } else if (is_init_free_available()) {
                init_free_arrival(t_, theta);
            } else {
                cold_start_arrival(t_, theta);
            }

            next_arrival_ = t_ + arrival_.sample();

            update_state_vector();
            // REMOVED: algo_.advance_to(t_); - Python doesn't call this
            calculate_cost();  // FIXED: Use wrapper method
            algo_.simulate_step(state_, *this);
            continue;
        }

        // Instance event is earlier
        t_ = next_event_t;
        hist_.times.push_back(t_);
        update_hist_arrays(t_);

        auto old_state = servers_[min_idx].state();
        auto new_state = servers_[min_idx].make_transition();

        if (new_state == FunctionState::COLD) {
            if (old_state == FunctionState::IDLE_ON) {
                --idle_count_;
            }

            // FIXED: Track lifespan
            stats_.sum_lifespan += (t_ - servers_[min_idx].creation_time());
            stats_.num_terminated += 1;

            // FIXED: Save to prev_servers for lifespan analysis
            prev_servers_.push_back(servers_[min_idx]);

            --server_count_;
            servers_.erase(servers_.begin() + min_idx);
        } else if (new_state == FunctionState::IDLE_ON) {
            if (old_state == FunctionState::BUSY) {
                --running_count_;
                ++idle_count_;
            } else if (old_state == FunctionState::INIT_FREE) {
                --init_free_count_;
                ++idle_count_;
                servers_[min_idx].unreserved();
            }
        } else if (new_state == FunctionState::BUSY) {
            if (old_state == FunctionState::INIT_RESERVED) {
                servers_[min_idx].update_next_transition(t_);
                --init_reserved_count_;
                ++running_count_;
            } else if (old_state == FunctionState::INIT_FREE && servers_[min_idx].is_reserved()) {
                servers_[min_idx].update_next_transition(t_);
                servers_[min_idx].unreserved();

                --init_free_booked_;
                --queued_jobs_count_;
                ++running_count_;
            }
        }

        update_state_vector();
        // REMOVED: algo_.advance_to(t_); - Python doesn't call this
        calculate_cost();  // FIXED: Use wrapper method
        algo_.simulate_step(state_, *this);
    }
}

// NEW: Calculate time lengths for weighted statistics
void ServerlessSimulator::calculate_time_lengths() const {
    time_lengths_.clear();
    if (hist_.times.size() < 2) return;

    time_lengths_.reserve(hist_.times.size() - 1);
    for (size_t i = 1; i < hist_.times.size(); ++i) {
        time_lengths_.push_back(hist_.times[i] - hist_.times[i-1]);
    }
}

// NEW: Get trace end time
double ServerlessSimulator::get_trace_end() const {
    if (hist_.times.empty()) return 0.0;
    return hist_.times.back();
}

// FIXED: Time-weighted average instead of arithmetic mean
double ServerlessSimulator::get_average_server_count() const {
    if (time_lengths_.empty() || hist_.server_count.empty()) return 0.0;
    if (get_trace_end() <= 0.0) return 0.0;

    double weighted_sum = 0.0;
    size_t n = std::min(time_lengths_.size(), hist_.server_count.size());
    for (size_t i = 0; i < n; ++i) {
        weighted_sum += hist_.server_count[i] * time_lengths_[i];
    }
    return weighted_sum / get_trace_end();
}

double ServerlessSimulator::get_average_server_running_count() const {
    if (time_lengths_.empty() || hist_.running_count.empty()) return 0.0;
    if (get_trace_end() <= 0.0) return 0.0;

    double weighted_sum = 0.0;
    size_t n = std::min(time_lengths_.size(), hist_.running_count.size());
    for (size_t i = 0; i < n; ++i) {
        weighted_sum += hist_.running_count[i] * time_lengths_[i];
    }
    return weighted_sum / get_trace_end();
}

double ServerlessSimulator::get_average_server_idle_count() const {
    if (time_lengths_.empty() || hist_.idle_count.empty()) return 0.0;
    if (get_trace_end() <= 0.0) return 0.0;

    double weighted_sum = 0.0;
    size_t n = std::min(time_lengths_.size(), hist_.idle_count.size());
    for (size_t i = 0; i < n; ++i) {
        weighted_sum += hist_.idle_count[i] * time_lengths_[i];
    }
    return weighted_sum / get_trace_end();
}

double ServerlessSimulator::get_average_server_init_free_count() const {
    if (time_lengths_.empty() || hist_.init_free_count.empty()) return 0.0;
    if (get_trace_end() <= 0.0) return 0.0;

    double weighted_sum = 0.0;
    size_t n = std::min(time_lengths_.size(), hist_.init_free_count.size());
    for (size_t i = 0; i < n; ++i) {
        weighted_sum += hist_.init_free_count[i] * time_lengths_[i];
    }
    return weighted_sum / get_trace_end();
}

double ServerlessSimulator::get_average_server_init_reserved_count() const {
    if (time_lengths_.empty() || hist_.init_reserved_count.empty()) return 0.0;
    if (get_trace_end() <= 0.0) return 0.0;

    double weighted_sum = 0.0;
    size_t n = std::min(time_lengths_.size(), hist_.init_reserved_count.size());
    for (size_t i = 0; i < n; ++i) {
        weighted_sum += hist_.init_reserved_count[i] * time_lengths_[i];
    }
    return weighted_sum / get_trace_end();
}

double ServerlessSimulator::get_average_server_queued_jobs_count() const {
    if (time_lengths_.empty() || hist_.queued_job_count.empty()) return 0.0;
    if (get_trace_end() <= 0.0) return 0.0;

    double weighted_sum = 0.0;
    size_t n = std::min(time_lengths_.size(), hist_.queued_job_count.size());
    for (size_t i = 0; i < n; ++i) {
        weighted_sum += hist_.queued_job_count[i] * time_lengths_[i];
    }
    return weighted_sum / get_trace_end();
}

double ServerlessSimulator::get_average_lifespan() const {
    if (prev_servers_.empty()) return 0.0;

    double sum = 0.0;
    for (const auto& s : prev_servers_) {
        // Lifespan = termination time - creation time
        // We saved them at termination, so we need to track this properly
        // This is simplified; in production you'd store termination time
        sum += stats_.sum_lifespan;
    }

    // Actually use the tracked sum
    return stats_.num_terminated > 0 ? stats_.sum_lifespan / stats_.num_terminated : 0.0;
}

double ServerlessSimulator::get_cold_start_prob() const {
    return stats_.total_req > 0 ? (double)stats_.total_cold / stats_.total_req : 0.0;
}

// NEW: Get result dictionary
ServerlessSimulator::ResultDict ServerlessSimulator::get_result_dict() {
    calculate_time_lengths();  // Ensure time lengths are calculated

    return ResultDict{
        .reqs_cold = stats_.total_cold,
        .reqs_total = stats_.total_req,
        .reqs_init_free = stats_.total_init_free,
        .reqs_init_reserved = stats_.total_init_reserved,
        .reqs_warm = stats_.total_warm,
        .reqs_queued = stats_.total_queued,
        .reqs_reject = stats_.total_reject,
        .prob_cold = get_cold_start_prob(),
        .prob_reject = stats_.total_req > 0 ? (double)stats_.total_reject / stats_.total_req : 0.0,
        .lifespan_avg = get_average_lifespan(),
        .inst_count_avg = get_average_server_count(),
        .inst_running_count_avg = get_average_server_running_count(),
        .inst_idle_count_avg = get_average_server_idle_count(),
        .inst_init_free_count_avg = get_average_server_init_free_count(),
        .inst_init_reserved_count_avg = get_average_server_init_reserved_count(),
        .inst_queued_jobs_count_avg = get_average_server_queued_jobs_count()
    };
}

// FIXED: Corrected method name and implementation
void ServerlessSimulator::print_summary() const {
    calculate_time_lengths();  // Must calculate first

    const double total_req = static_cast<double>(stats_.total_req);
    const double p_cold = (total_req > 0) ? (double)stats_.total_cold / total_req : 0.0;
    const double p_rej = (total_req > 0) ? (double)stats_.total_reject / total_req : 0.0;

    // FIXED: Use time-weighted averages
    double avg_server = get_average_server_count();
    double avg_run = get_average_server_running_count();
    double avg_idle = get_average_server_idle_count();
    double avg_ifree = get_average_server_init_free_count();
    double avg_ires = get_average_server_init_reserved_count();
    double avg_qjobs = get_average_server_queued_jobs_count();
    double avg_life = get_average_lifespan();

    std::cout << std::fixed << std::setprecision(6);
    std::cout << "Cold starts / total requests: " << stats_.total_cold << " / " << stats_.total_req << "\n";
    std::cout << "Cold Start Probability:       " << p_cold << "\n";
    std::cout << "Rejection / total requests:   " << stats_.total_reject << " / " << stats_.total_req << "\n";
    std::cout << "Rejection Probability:        " << p_rej << "\n";
    std::cout << "Average Instance Life Span:   " << avg_life << "\n";
    std::cout << "Average Server Count:         " << avg_server << "\n";
    std::cout << "Average Running Count:        " << avg_run << "\n";
    std::cout << "Average Idle Count:           " << avg_idle << "\n";
    std::cout << "Average Init Free Count:      " << avg_ifree << "\n";
    std::cout << "Average Init Reserved Count:  " << avg_ires << "\n";
    std::cout << "Average Queued Jobs Count:    " << avg_qjobs << "\n";
    std::cout << "Total Init Free Count:        " << stats_.total_init_free << "\n";
    std::cout << "Total Init Reserved Count:    " << stats_.total_init_reserved << "\n";
    std::cout << "Total Queued Jobs Count:      " << stats_.total_queued << "\n";
}

// NEW: Save results to CSV
void ServerlessSimulator::save_results_to_csv(const std::string& log_dir) const {
    // Save theta, states, and costs if autoscaler provides them
    // This would require accessing autoscaler's internal data
    // Placeholder implementation - extend based on your AutoScalingAlgorithm interface

    // Example: Save history to CSV
    std::ofstream hist_file(log_dir + "/history.csv");
    if (hist_file.is_open()) {
        hist_file << "time,server_count,running_count,idle_count,init_free_count,init_reserved_count,queued_jobs\n";
        for (size_t i = 0; i < hist_.times.size(); ++i) {
            hist_file << hist_.times[i] << ","
                     << hist_.server_count[i] << ","
                     << hist_.running_count[i] << ","
                     << hist_.idle_count[i] << ","
                     << hist_.init_free_count[i] << ","
                     << hist_.init_reserved_count[i] << ","
                     << hist_.queued_job_count[i] << "\n";
        }
        hist_file.close();
    }
}
