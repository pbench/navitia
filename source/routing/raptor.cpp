/* Copyright © 2001-2014, Canal TP and/or its affiliates. All rights reserved.

This file is part of Navitia,
    the software to build cool stuff with public transport.

Hope you'll enjoy and contribute to this project,
    powered by Canal TP (www.canaltp.fr).
Help us simplify mobility and open public transport:
    a non ending quest to the responsive locomotion way of traveling!

LICENCE: This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.

Stay tuned using
twitter @navitia
channel `#navitia` on riot https://riot.im/app/#/room/#navitia:matrix.org
https://groups.google.com/d/forum/navitia
www.navitia.io
*/

#include "raptor.h"

#include "raptor_visitors.h"
#include "utils/logger.h"

#include <boost/functional/hash.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/algorithm/fill.hpp>
#include <boost/range/algorithm/find_if.hpp>
#include <boost/range/algorithm_ext/push_back.hpp>

#include <chrono>

namespace navitia {
namespace routing {

DateTime limit_bound(const bool clockwise, const DateTime departure_datetime, const DateTime bound) {
    auto depart_clockwise = departure_datetime + DateTimeUtils::SECONDS_PER_DAY;
    auto depart_anticlockwise =
        departure_datetime > DateTimeUtils::SECONDS_PER_DAY ? departure_datetime - DateTimeUtils::SECONDS_PER_DAY : 0;
    return clockwise ? std::min(depart_clockwise, bound) : std::max(depart_anticlockwise, bound);
}

/*
 * Check if the given vj is valid for the given datetime,
 * If it is for every stoptime of the vj,
 * If the journey_pattern_point associated to it is improved by this stop time
 * we mark it.
 * If the given vj also has an extension we apply it.
 */
template <typename Visitor>
bool RAPTOR::apply_vj_extension(const Visitor& v,
                                const nt::RTLevel rt_level,
                                const type::VehicleJourney* vj,
                                const uint16_t l_zone,
                                DateTime base_dt,
                                DateTime working_walking_duration,
                                SpIdx boarding_stop_point) {
    log4cplus::Logger logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("logger"));
    auto& working_labels = labels[count];
    bool result = false;
    while (vj) {
        base_dt = v.get_base_dt_extension(base_dt, vj);
        const auto& stop_time_list = v.stop_time_list(vj);
        const auto& st_begin = stop_time_list.front();
        const auto first_dt = st_begin.section_end(base_dt, v.clockwise());

        // If the vj is not valid for the first stop it won't be valid at all
        if (!st_begin.is_valid_day(DateTimeUtils::date(first_dt), !v.clockwise(), rt_level)) {
            return result;
        }
        for (const type::StopTime& st : stop_time_list) {
            const auto workingDt = st.section_end(base_dt, v.clockwise());
            if (!st.valid_end(v.clockwise())) {
                continue;
            }
            if (l_zone != std::numeric_limits<uint16_t>::max() && l_zone == st.local_traffic_zone) {
                continue;
            }
            auto sp = st.stop_point;
            const auto sp_idx = SpIdx(*sp);

            if (!valid_stop_points[sp_idx.val]) {
                continue;
            }
            if (!v.comp(workingDt, best_labels_pts[sp_idx])) {
                continue;
            }
            LOG4CPLUS_TRACE(logger, "Updating label dt count : " << count 
                                    << " sp " << data.pt_data->stop_points[sp_idx.val]->uri
                                    << " from " << working_labels.dt_pt(sp_idx)
                                    << " to "  << workingDt
                                    << " throught : " << st.vehicle_journey->route->line->uri
                                    << " boarding_stop_point : " << data.pt_data->stop_points[boarding_stop_point.val]->uri
                                    << " walk : " << working_walking_duration
                            );
            working_labels.mut_dt_pt(sp_idx) = workingDt;
            working_labels.mut_walking_duration_pt(sp_idx) = working_walking_duration;
            BOOST_ASSERT(working_walking_duration != DateTimeUtils::not_valid);
            best_labels_pts[sp_idx] = workingDt;
            result = true;
        }
        vj = v.get_extension_vj(vj);
    }
    return result;
}

template <typename Visitor>
bool RAPTOR::foot_path(const Visitor& v) {
    log4cplus::Logger logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("logger"));
    bool result = false;
    auto& working_labels = labels[count];
    const auto& cnx_list = v.clockwise() ? data.dataRaptor->connections.forward_connections
                                         : data.dataRaptor->connections.backward_connections;

    for (const auto sp_cnx : cnx_list) {
        // for all stop point, we check if we can improve the stop points they are in connection with
        const auto& sp_idx = sp_cnx.first;

        if (!working_labels.pt_is_initialized(sp_idx)) {
            continue;
        }

        const DateTime previous_dt = working_labels.dt_pt(sp_idx);
        const DateTime previous_walking_duration_dt = working_labels.walking_duration_pt(sp_idx);

        for (const auto& conn : sp_cnx.second) {
            const SpIdx destination_sp_idx = conn.sp_idx;
            const DateTime next = v.combine(previous_dt, conn.duration);

            if (!v.comp(next, best_labels_transfers[destination_sp_idx])) {
                continue;
            }

            LOG4CPLUS_TRACE(logger, "Updating label transfer count : " << count 
                        << " sp " << data.pt_data->stop_points[destination_sp_idx.val]->uri
                        << " from " << working_labels.dt_transfer(destination_sp_idx)
                        << " to "  << next
                        << " throught connection : " << data.pt_data->stop_points[sp_idx.val]->uri
                        << " walk : " << previous_walking_duration_dt
                );

            // if we can improve the best label, we mark it
            working_labels.mut_dt_transfer(destination_sp_idx) = next;
            working_labels.mut_walking_duration_transfer(destination_sp_idx) = previous_walking_duration_dt + conn.duration;
            best_labels_transfers[destination_sp_idx] = next;
            result = true;
        }
    }

    for (const auto sp_jpps : jpps_from_sp) {
        if (!working_labels.transfer_is_initialized(sp_jpps.first)) {
            continue;
        }

        // we mark the jpp order
        for (const auto& jpp : sp_jpps.second) {
            if (v.comp(jpp.order, Q[jpp.jp_idx])) {
                Q[jpp.jp_idx] = jpp.order;
            }
        }
    }

    return result;
}

void RAPTOR::clear(const bool clockwise, const DateTime bound) {
    const int queue_value = clockwise ? std::numeric_limits<int>::max() : -1;
    Q.assign(data.dataRaptor->jp_container.get_jps_values(), queue_value);
    if (labels.empty()) {
        labels.resize(5);
    }
    const Labels& clean_labels = clockwise ? data.dataRaptor->labels_const : data.dataRaptor->labels_const_reverse;
    for (auto& lbl_list : labels) {
        lbl_list.clear(clean_labels);
    }

    boost::fill(best_labels_pts.values(), bound);
    boost::fill(best_labels_transfers.values(), bound);
}

void RAPTOR::init(const map_stop_point_duration& dep,
                  const DateTime bound,
                  const bool clockwise,
                  const type::Properties& properties) {
    for (const auto& sp_dt : dep) {
        if (!get_sp(sp_dt.first)->accessible(properties)) {
            continue;
        }
        const DateTime sn_dur = sp_dt.second.total_seconds();
        const DateTime begin_dt = bound + (clockwise ? sn_dur : -sn_dur);
        labels[0].mut_dt_transfer(sp_dt.first) = begin_dt;
        labels[0].mut_walking_duration_transfer(sp_dt.first) = sn_dur;
        best_labels_transfers[sp_dt.first] = begin_dt;
        for (const auto& jpp : jpps_from_sp[sp_dt.first]) {
            if (clockwise && Q[jpp.jp_idx] > jpp.order) {
                Q[jpp.jp_idx] = jpp.order;
            } else if (!clockwise && Q[jpp.jp_idx] < jpp.order) {
                Q[jpp.jp_idx] = jpp.order;
            }
        }
    }
}

void RAPTOR::first_raptor_loop(const map_stop_point_duration& departures,
                               const DateTime& departure_datetime,
                               const nt::RTLevel rt_level,
                               const DateTime& bound_limit,
                               const uint32_t max_transfers,
                               const type::AccessibiliteParams& accessibilite_params,
                               const bool clockwise) {
    const DateTime bound = limit_bound(clockwise, departure_datetime, bound_limit);

    assert(data.dataRaptor->cached_next_st_manager);
    next_st = data.dataRaptor->cached_next_st_manager->load(clockwise ? departure_datetime : bound, rt_level,
                                                            accessibilite_params);

    clear(clockwise, bound);
    init(departures, departure_datetime, clockwise, accessibilite_params.properties);

    boucleRAPTOR(clockwise, rt_level, max_transfers);
}

namespace {
struct Dom {
    explicit Dom(bool c) : clockwise(c) {}
    bool clockwise;
    using Arg = std::pair<size_t, StartingPointSndPhase>;
    inline bool operator()(const Arg& lhs, const Arg& rhs) const {
        /*
         * When multiple arrival with [same walking, same time, same number of sections] are
         * possible we keep them all as they are equally interesting
         */
        if (lhs.second.count == rhs.second.count && lhs.second.end_dt == rhs.second.end_dt
            && lhs.second.fallback_dur == rhs.second.fallback_dur) {
            return false;
        }
        return lhs.second.count <= rhs.second.count
               && (clockwise ? lhs.second.end_dt <= rhs.second.end_dt : lhs.second.end_dt >= rhs.second.end_dt)
               && lhs.second.fallback_dur <= rhs.second.fallback_dur;
    }
};
struct CompSndPhase {
    explicit CompSndPhase(bool c) : clockwise(c) {}
    bool clockwise;
    using Arg = StartingPointSndPhase;
    inline bool operator()(const Arg& lhs, const Arg& rhs) const {
        if (lhs.has_priority != rhs.has_priority) {
            return lhs.has_priority;
        }
        if (lhs.count != rhs.count) {
            return lhs.count < rhs.count;
        }
        if (lhs.end_dt != rhs.end_dt) {
            return (clockwise ? lhs.end_dt < rhs.end_dt : lhs.end_dt > rhs.end_dt);
        }
        if (lhs.fallback_dur != rhs.fallback_dur) {
            return lhs.fallback_dur < rhs.fallback_dur;
        }
        return lhs.sp_idx < rhs.sp_idx;  // just to avoid randomness
    }
};

std::vector<StartingPointSndPhase> make_starting_points_snd_phase(const RAPTOR& raptor,
                                                                  const routing::map_stop_point_duration& arrs,
                                                                  const type::AccessibiliteParams& accessibilite_params,
                                                                  const bool clockwise) {
    std::vector<StartingPointSndPhase> res;
    auto overfilter = ParetoFront<std::pair<size_t, StartingPointSndPhase>, Dom>(Dom(clockwise));
    log4cplus::Logger logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("raptor"));
    
    for (unsigned count = 1; count <= raptor.count; ++count) {
        const auto& working_labels = raptor.labels[count];
        for (const auto& a : arrs) {
            if (!working_labels.pt_is_initialized(a.first)) {
                continue;
            }
            if (!raptor.get_sp(a.first)->accessible(accessibilite_params.properties)) {
                continue;
            }

            const unsigned arrival_walking_t = a.second.total_seconds();
            const DateTime walking_duration_before_arrival_stop_point = working_labels.walking_duration_pt(a.first);
            const DateTime total_walking_duration = arrival_walking_t + walking_duration_before_arrival_stop_point;
            StartingPointSndPhase starting_point = {
                a.first, count,
                (clockwise ? working_labels.dt_pt(a.first) + arrival_walking_t : working_labels.dt_pt(a.first) - arrival_walking_t),
                total_walking_duration, false};
            
            // LOG4CPLUS_TRACE(logger, "Candidate starting point second phase : " 
            //                     << raptor.data.pt_data->stop_points[a.first.val]->uri
            //                     << std::endl
            //                     << " count : " << count
            //                     << " arrival walking time : " << arrival_walking_t
            //                     << " walking_before_arrival : " << walking_duration_before_arrival_stop_point
            //                 );


            overfilter.add({res.size(), starting_point});
            res.push_back(starting_point);
        }
    }

    for (const auto& pair : overfilter) {
        res[pair.first].has_priority = true;
    }

    std::sort(res.begin(), res.end(), CompSndPhase(clockwise));  // most interesting solutions first

    return res;
}

// creation of a fake journey from informations known after first pass
// this journey aims at being the best we can hope from this StartingPointSndPhase
Journey convert_to_bound(const StartingPointSndPhase& sp,
                         uint32_t lower_bound_fb,
                         uint32_t lower_bound_conn,
                         const navitia::time_duration& transfer_penalty,
                         bool clockwise) {
    Journey journey;
    journey.sections.resize(sp.count);  // only the number of sections is part of the dominance function
    journey.sn_dur = navitia::time_duration(0, 0, sp.fallback_dur, 0);
    uint32_t nb_conn = (sp.count >= 1 ? sp.count - 1 : 0);
    if (clockwise) {
        journey.arrival_dt = sp.end_dt;
        journey.departure_dt = sp.end_dt - journey.sn_dur.seconds(); 
    } else {
        journey.arrival_dt = sp.end_dt + journey.sn_dur.seconds(); 
        journey.departure_dt = sp.end_dt;
    }

    journey.transfer_dur = transfer_penalty * sp.count + navitia::seconds(nb_conn * lower_bound_conn);
    // provide best values on unknown criteria
    journey.min_waiting_dur = navitia::time_duration(boost::date_time::pos_infin);
    journey.nb_vj_extentions = 0;
    return journey;
}

// To be used for debug purpose (see JourneyParetoFrontVisitor commented use in RAPTOR::compute_all)
//
// struct JourneyParetoFrontVisitor {
//    JourneyParetoFrontVisitor() = default;
//    JourneyParetoFrontVisitor(const JourneyParetoFrontVisitor&) = default;
//    JourneyParetoFrontVisitor& operator=(const JourneyParetoFrontVisitor&) = default;
//
//    void is_dominated_by(const Journey& to_insert, const Journey& /*front_cur*/) {
//        LOG4CPLUS_DEBUG(logger, "[  ]JourneyParetoFront " << to_insert);
//        ++nb_is_dominated_by;
//    }
//    void dominates(const Journey& /*to_insert*/, const Journey& front_cur) {
//        LOG4CPLUS_DEBUG(logger, "[--]JourneyParetoFront " << front_cur);
//        ++nb_dominates;
//    }
//    void inserted(const Journey& to_insert) {
//        LOG4CPLUS_DEBUG(logger, "[++]JourneyParetoFront " << to_insert);
//        ++nb_inserted;
//    }
//
//    ~JourneyParetoFrontVisitor() {
//        LOG4CPLUS_DEBUG(logger, "JourneyParetoFront summary: nb_is_dominated_by = " << nb_is_dominated_by
//                << ", nb_dominates = " << nb_dominates
//                << ", nb_inserted = " << nb_inserted);
//
//    }
//
// private:
//    size_t nb_is_dominated_by = 0;
//    size_t nb_dominates = 0;
//    size_t nb_inserted = 0;
//    log4cplus::Logger logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("logger"));
//};
}  // namespace

// copy and do the off by one for strict comparison for the second pass.
static IdxMap<type::StopPoint, DateTime> snd_pass_best_labels(const bool clockwise,
                                                              IdxMap<type::StopPoint, DateTime> best_labels) {
    for (auto& dt : best_labels.values()) {
        if (is_dt_initialized(dt)) {
            dt += clockwise ? -1 : 1;
        }
    }
    return best_labels;
}
// Set the departure bounds on best_labels_pts for the second pass.
static void init_best_pts_snd_pass(const routing::map_stop_point_duration& departures,
                                   const DateTime& departure_datetime,
                                   const bool clockwise,
                                   IdxMap<type::StopPoint, DateTime>& best_labels) {
    for (const auto& d : departures) {
        if (clockwise) {
            best_labels[d.first] =
                std::min(best_labels[d.first], uint(departure_datetime + d.second.total_seconds() - 1));
        } else {
            best_labels[d.first] =
                std::max(best_labels[d.first], uint(departure_datetime - d.second.total_seconds() + 1));
        }
    }
}

std::vector<Path> RAPTOR::compute_all(const map_stop_point_duration& departures,
                                      const map_stop_point_duration& destinations,
                                      const DateTime& departure_datetime,
                                      const nt::RTLevel rt_level,
                                      const navitia::time_duration& transfer_penalty,
                                      const DateTime& bound,
                                      const uint32_t max_transfers,
                                      const type::AccessibiliteParams& accessibilite_params,
                                      const std::vector<std::string>& forbidden_uri,
                                      const std::vector<std::string>& allowed_ids,
                                      bool clockwise,
                                      const boost::optional<navitia::time_duration>& direct_path_dur,
                                      const size_t max_extra_second_pass) {
    set_valid_jp_and_jpp(DateTimeUtils::date(departure_datetime), accessibilite_params, forbidden_uri, allowed_ids,
                         rt_level);

    const auto& journeys =
        compute_all_journeys(departures, destinations, departure_datetime, rt_level, transfer_penalty, bound,
                             max_transfers, accessibilite_params, clockwise, direct_path_dur, max_extra_second_pass);
    return from_journeys_to_path(journeys);
}

RAPTOR::Journeys RAPTOR::compute_all_journeys(const map_stop_point_duration& departures,
                                              const map_stop_point_duration& destinations,
                                              const DateTime& departure_datetime,
                                              const nt::RTLevel rt_level,
                                              const navitia::time_duration& transfer_penalty,
                                              const DateTime& bound,
                                              const uint32_t max_transfers,
                                              const type::AccessibiliteParams& accessibilite_params,
                                              bool clockwise,
                                              const boost::optional<navitia::time_duration>& direct_path_dur,
                                              const size_t max_extra_second_pass) {

    log4cplus::Logger logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("logger"));

    LOG4CPLUS_TRACE(logger, "departure_datetime : " << iso_string(departure_datetime, data)
                            << " bound : " << iso_string(bound, data)
                    );

    auto start_raptor = std::chrono::system_clock::now();

    //auto solutions = ParetoFront<Journey, Dominates /*, JourneyParetoFrontVisitor*/>(Dominates(clockwise));
    auto dominator = Dominates(clockwise);
    auto solutions = Solutions(dominator);

    if (direct_path_dur) {
        Journey j;
        j.sn_dur = *direct_path_dur;
        if (clockwise) {
            j.departure_dt = departure_datetime;
            j.arrival_dt = j.departure_dt + j.sn_dur;
        } else {
            j.arrival_dt = departure_datetime;
            j.departure_dt = j.arrival_dt - j.sn_dur;
        }
        solutions.add(j);
        LOG4CPLUS_INFO(logger, "adding direct path to solutions : " << std::endl << j);
    }

    const auto& calc_dep = clockwise ? departures : destinations;
    const auto& calc_dest = clockwise ? destinations : departures;

    first_raptor_loop(calc_dep, departure_datetime, rt_level, bound, max_transfers, accessibilite_params, clockwise);

    // LOG4CPLUS_TRACE(logger, "labels after first pass : " << std::endl << print_all_labels() );

    // LOG4CPLUS_TRACE(logger, print_best_labels() );

    auto end_first_pass = std::chrono::system_clock::now();

    // LOG4CPLUS_TRACE(logger, "end first pass");

    // Now, we do the second pass.  In case of clockwise (resp
    // anticlockwise) search, the goal of the second pass is to find
    // the earliest (resp. tardiest) departure (resp arrival)
    // datetime.  For each count and arrival (resp departure), we
    // launch a backward raptor.
    //
    // As we do a backward raptor, the bound computed during the first
    // pass can be used in the second pass.  The arrival at a stop
    // point (as in best_labels_transfers) is a bound to the get in
    // (as in best_labels_pt) in the second pass.  Then, we can reuse
    // these bounds, modulo an off by one because of strict comparison
    // on best_labels.
    auto starting_points = make_starting_points_snd_phase(*this, calc_dest, accessibilite_params, clockwise);
    swap(labels, first_pass_labels);
    auto best_labels_pts_for_snd_pass = snd_pass_best_labels(clockwise, best_labels_transfers);
    init_best_pts_snd_pass(calc_dep, departure_datetime, clockwise, best_labels_pts_for_snd_pass);
    auto best_labels_transfers_for_snd_pass = snd_pass_best_labels(clockwise, best_labels_pts);

    LOG4CPLUS_TRACE(logger, "starting points 2nd phase " << std::endl << print_starting_points_snd_phase(starting_points));

    unsigned lower_bound_fb = std::numeric_limits<unsigned>::max();
    for (const auto& pair_sp_dt : calc_dep) {
        lower_bound_fb = std::min(lower_bound_fb, unsigned(pair_sp_dt.second.seconds()));
    }

    LOG4CPLUS_TRACE(logger, "Before second pass, nb of solutions : " << solutions.size());

    size_t nb_snd_pass = 0, nb_useless = 0, last_usefull_2nd_pass = 0, supplementary_2nd_pass = 0;
    for (const auto& start : starting_points) {
        navitia::type::StopPoint* start_stop_point = data.pt_data->stop_points[start.sp_idx.val];

        LOG4CPLUS_TRACE(logger, std::endl << "Second pass from " << start_stop_point->uri << "   count : " << start.count
                                    << std::endl << std::endl
                                    // << "   count : " << start.count
                                    // << " end_dt : " << iso_string(start.end_dt, data)
                                    // << " fallback_dur : " << start.fallback_dur
                                    // << " has_priority : " << start.has_priority
                        );

        Journey fake_journey =
            convert_to_bound(start, lower_bound_fb, data.dataRaptor->min_connection_time, transfer_penalty, clockwise);
        
        LOG4CPLUS_TRACE(logger," fake journey : " << fake_journey);
        

        if (solutions.contains_better_than(fake_journey)) {
            LOG4CPLUS_TRACE(logger, "has better solution than fake journey from " << start_stop_point->uri);
            for(auto solution : solutions) {
                if (dominator(solution, fake_journey)) {
                    LOG4CPLUS_TRACE(logger,"  dominated by : " << solution);
                    break;
                }
            }
            continue;
        }

        if (!start.has_priority) {
            ++supplementary_2nd_pass;
        }
        if (supplementary_2nd_pass > max_extra_second_pass) {
            LOG4CPLUS_TRACE(logger, "max second pass reached");
            break;
        }

        const auto& working_labels = first_pass_labels[start.count];

        clear(!clockwise, departure_datetime + (clockwise ? -1 : 1));
        map_stop_point_duration init_map;
        init_map[start.sp_idx] = 0_s;
        best_labels_pts = best_labels_pts_for_snd_pass;
        best_labels_transfers = best_labels_transfers_for_snd_pass;
        init(init_map, working_labels.dt_pt(start.sp_idx), !clockwise, accessibilite_params.properties);
        boucleRAPTOR(!clockwise, rt_level, max_transfers);

        // LOG4CPLUS_TRACE(logger, std::endl << "before solution_reader " << std::endl << print_all_labels() );

        read_solutions(*this, solutions, !clockwise, departure_datetime, departures, destinations, rt_level,
                       accessibilite_params, transfer_penalty, start);

        LOG4CPLUS_TRACE(logger, "end of loop body, nb of solutions : " << solutions.size());

        ++nb_snd_pass;
    }

    LOG4CPLUS_DEBUG(logger, "[2nd pass] lower bound fallback duration = "
                                << lower_bound_fb << " s, lower bound connection duration = "
                                << data.dataRaptor->min_connection_time << " s");
    LOG4CPLUS_DEBUG(logger, "[2nd pass] number of 2nd pass = "
                                << nb_snd_pass << " / " << starting_points.size() << " (nb useless = " << nb_useless
                                << ", last usefull try = " << last_usefull_2nd_pass << ")");
    auto end_raptor = std::chrono::system_clock::now();
    LOG4CPLUS_DEBUG(logger,
                    "[2nd pass] Run times: 1st pass = "
                        << std::chrono::duration_cast<std::chrono::milliseconds>(end_first_pass - start_raptor).count()
                        << ", 2nd pass = "
                        << std::chrono::duration_cast<std::chrono::milliseconds>(end_raptor - end_first_pass).count());

    // return raw results
    return solutions.get_pool();
}

void RAPTOR::isochrone(const map_stop_point_duration& departures,
                       const DateTime& departure_datetime,
                       const DateTime& b,
                       const uint32_t max_transfers,
                       const type::AccessibiliteParams& accessibilite_params,
                       const std::vector<std::string>& forbidden,
                       const std::vector<std::string>& allowed,
                       const bool clockwise,
                       const nt::RTLevel rt_level) {
    set_valid_jp_and_jpp(DateTimeUtils::date(departure_datetime), accessibilite_params, forbidden, allowed, rt_level);

    first_raptor_loop(departures, departure_datetime, rt_level, b, max_transfers, accessibilite_params, clockwise);
}

namespace {
struct ObjsFromIds {
    boost::dynamic_bitset<> jps;
    boost::dynamic_bitset<> jpps;
    boost::dynamic_bitset<> sps;
    ObjsFromIds(const std::vector<std::string>& ids, const JourneyPatternContainer& jp_container, const nt::Data& data)
        : jps(jp_container.nb_jps()), jpps(jp_container.nb_jpps()), sps(data.pt_data->stop_points.size()) {
        for (const auto& id : ids) {
            const auto it_line = data.pt_data->lines_map.find(id);
            if (it_line != data.pt_data->lines_map.end()) {
                for (const auto route : it_line->second->route_list) {
                    for (const auto& jp_idx : jp_container.get_jps_from_route()[RouteIdx(*route)]) {
                        jps.set(jp_idx.val, true);
                    }
                }
                continue;
            }
            const auto it_route = data.pt_data->routes_map.find(id);
            if (it_route != data.pt_data->routes_map.end()) {
                for (const auto& jp_idx : jp_container.get_jps_from_route()[RouteIdx(*it_route->second)]) {
                    jps.set(jp_idx.val, true);
                }
                continue;
            }
            const auto it_commercial_mode = data.pt_data->commercial_modes_map.find(id);
            if (it_commercial_mode != data.pt_data->commercial_modes_map.end()) {
                for (const auto line : it_commercial_mode->second->line_list) {
                    for (auto route : line->route_list) {
                        for (const auto& jp_idx : jp_container.get_jps_from_route()[RouteIdx(*route)]) {
                            jps.set(jp_idx.val, true);
                        }
                    }
                }
                continue;
            }
            const auto it_physical_mode = data.pt_data->physical_modes_map.find(id);
            if (it_physical_mode != data.pt_data->physical_modes_map.end()) {
                const auto phy_mode_idx = PhyModeIdx(*it_physical_mode->second);
                for (const auto& jp_idx : jp_container.get_jps_from_phy_mode()[phy_mode_idx]) {
                    jps.set(jp_idx.val, true);
                }
                continue;
            }
            const auto it_network = data.pt_data->networks_map.find(id);
            if (it_network != data.pt_data->networks_map.end()) {
                for (const auto line : it_network->second->line_list) {
                    for (const auto route : line->route_list) {
                        for (const auto& jp_idx : jp_container.get_jps_from_route()[RouteIdx(*route)]) {
                            jps.set(jp_idx.val, true);
                        }
                    }
                }
                continue;
            }
            const auto it_sp = data.pt_data->stop_points_map.find(id);
            if (it_sp != data.pt_data->stop_points_map.end()) {
                sps.set(it_sp->second->idx, true);
                for (const auto& jpp : data.dataRaptor->jpps_from_sp[SpIdx(*it_sp->second)]) {
                    jpps.set(jpp.idx.val, true);
                }
                continue;
            }
            const auto it_sa = data.pt_data->stop_areas_map.find(id);
            if (it_sa != data.pt_data->stop_areas_map.end()) {
                for (const auto sp : it_sa->second->stop_point_list) {
                    sps.set(sp->idx, true);
                    for (const auto& jpp : data.dataRaptor->jpps_from_sp[SpIdx(*sp)]) {
                        jpps.set(jpp.idx.val, true);
                    }
                }
                continue;
            }
        }
    }
};
}  // namespace

// Returns valid_jpps
void RAPTOR::set_valid_jp_and_jpp(uint32_t date,
                                  const type::AccessibiliteParams& accessibilite_params,
                                  const std::vector<std::string>& forbidden,
                                  const std::vector<std::string>& allowed,
                                  const nt::RTLevel rt_level) {
    const auto& jp_container = data.dataRaptor->jp_container;
    valid_journey_patterns = data.dataRaptor->jp_validity_patterns[rt_level][date];
    boost::dynamic_bitset<> valid_journey_pattern_points(jp_container.nb_jpps());
    valid_journey_pattern_points.set();
    valid_stop_points.set();

    auto forbidden_objs = ObjsFromIds(forbidden, jp_container, data);
    valid_journey_patterns &= forbidden_objs.jps.flip();
    valid_journey_pattern_points &= forbidden_objs.jpps.flip();
    valid_stop_points &= forbidden_objs.sps.flip();

    const auto allowed_objs = ObjsFromIds(allowed, jp_container, data);
    if (allowed_objs.jps.any()) {
        // If a journey pattern is present in allowed_obj, the
        // constraint is setted. Else, there is no constraint at the
        // journey pattern level.
        valid_journey_patterns &= allowed_objs.jps;
    }
    if (allowed_objs.jpps.any()) {
        // If a journey point pattern is present in allowed_obj, the
        // constraint is setted. Else, there is no constraint at the
        // journey pattern point level.
        valid_journey_pattern_points &= allowed_objs.jpps;
        valid_stop_points &= allowed_objs.sps;
    }

    // filter accessibility
    if (accessibilite_params.properties.any()) {
        for (const auto* sp : data.pt_data->stop_points) {
            if (sp->accessible(accessibilite_params.properties)) {
                continue;
            }
            valid_stop_points.set(sp->idx, false);
            for (const auto& jpp : data.dataRaptor->jpps_from_sp[SpIdx(*sp)]) {
                valid_journey_pattern_points.set(jpp.idx.val, false);
            }
        }
    }

    // propagate the invalid jp in their jpp
    for (JpIdx jp_idx = JpIdx(0); jp_idx.val < valid_journey_patterns.size(); ++jp_idx.val) {
        if (valid_journey_patterns[jp_idx.val]) {
            continue;
        }
        const auto& jp = jp_container.get(jp_idx);
        for (const auto& jpp_idx : jp.jpps) {
            valid_journey_pattern_points.set(jpp_idx.val, false);
        }
    }

    // We get our own copy of jpps_from_sp to filter every invalid
    // jpps.  Thanks to that, we don't need to check
    // valid_journey_pattern[_point]s as we iterate only on the
    // feasible ones.
    jpps_from_sp = data.dataRaptor->jpps_from_sp;
    jpps_from_sp.filter_jpps(valid_journey_pattern_points);
}

template <typename Visitor>
void RAPTOR::raptor_loop(Visitor visitor, const nt::RTLevel rt_level, uint32_t max_transfers) {
    log4cplus::Logger logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("logger"));
    bool continue_algorithm = true;
    count = 0;  //< Count iteration of raptor algorithm
 
    // LOG4CPLUS_TRACE(logger, "start raptor loop");

    while (continue_algorithm && count <= max_transfers) {
        // LOG4CPLUS_TRACE(logger, " in raptor, count : " << count );
        ++count;
        continue_algorithm = false;
        if (count == labels.size()) {
            if (visitor.clockwise()) {
                this->labels.push_back(this->data.dataRaptor->labels_const);
            } else {
                this->labels.push_back(this->data.dataRaptor->labels_const_reverse);
            }
        }
        const auto& prec_labels = labels[count - 1];
        auto& working_labels = labels[this->count];
        /*
         * We need to store it so we can apply stay_in after applying normal vjs
         * We want to do it, to favoritize normal vj against stay_in vjs
         */
        for (auto q_elt : Q) {

            /// We will scan the journey_pattern q_elt.first, starting from its stop numbered q_elt.second

            const JpIdx jp_idx = q_elt.first;

            const RouteIdx route_idx = data.dataRaptor->jp_container.get(jp_idx).route_idx;



            /// q_elt.second == visitor.init_queue_item() means that
            /// this journey_pattern is marked "not to be scanned"
            if (q_elt.second != visitor.init_queue_item()) {
                /// we begin scanning the journey_pattern as if we were not yet aboard a vehicle
                bool is_onboard = false;
                DateTime workingDt = visitor.worst_datetime();
                DateTime base_dt = workingDt;
                DateTime  working_walking_duration = DateTimeUtils::not_valid;
                SpIdx    boarding_stop_point = SpIdx();

                /// will be used to iterate through the StopTimeS of
                /// the vehicle journey of the current journey_pattern (jp_idx)
                ///  with the relevant departure date
                typename Visitor::stop_time_iterator it_st;  /// item = type::StopTime
                uint16_t l_zone = std::numeric_limits<uint16_t>::max();

                LOG4CPLUS_TRACE(logger, " Scanning line  " 
                        << data.pt_data->routes[route_idx.val]->line->uri
                        );


                const auto& jpps_to_explore =
                    visitor.jpps_from_order(data.dataRaptor->jpps_from_jp, jp_idx, q_elt.second);
                for (const auto& jpp : jpps_to_explore) {
                    /// jpp : dataRAPTOR::JppsFromJp::Jpp
                    if (is_onboard) {
                        ++it_st;
                        // We update workingDt with the new arrival time
                        // We need at each journey pattern point when we have a st
                        // If we don't it might cause problem with overmidnight vj
                        const type::StopTime& st = *it_st;
                        workingDt = st.section_end(base_dt, visitor.clockwise());
                        // We check if there are no drop_off_only and if the local_zone is okay
                        if (st.valid_end(visitor.clockwise())
                            && (l_zone == std::numeric_limits<uint16_t>::max() || l_zone != st.local_traffic_zone)
                            && visitor.comp(workingDt, best_labels_pts[jpp.sp_idx])
                            && valid_stop_points[jpp.sp_idx.val])  // we need to check the accessibility
                        {

                            LOG4CPLUS_TRACE(logger, "Updating label dt count : " << count 
                                                    << " sp " << data.pt_data->stop_points[jpp.sp_idx.val]->uri
                                                    << " from " << working_labels.dt_pt(jpp.sp_idx)
                                                    << " to "  << workingDt
                                                    << " throught : " << st.vehicle_journey->route->line->uri
                                                    << " boarding_stop_point : " << data.pt_data->stop_points[boarding_stop_point.val]->uri
                                                    << " walk : " << working_walking_duration
                                           );

                            working_labels.mut_dt_pt(jpp.sp_idx) = workingDt;
                            working_labels.mut_walking_duration_pt(jpp.sp_idx) = working_walking_duration;
                            BOOST_ASSERT(working_walking_duration != DateTimeUtils::not_valid);
                            /// TODO : update working_labels.walking_duration_dt_pt
                            best_labels_pts[jpp.sp_idx] = working_labels.dt_pt(jpp.sp_idx);
                            continue_algorithm = true;
                        }
                    }



                    // We try to get on a vehicle, if we were already on a vehicle, but we arrived
                    // before on the previous via a connection, we try to catch a vehicle leaving this
                    // journey pattern point before


                    // if we cannot board at this stop point, nothing to do
                    if (  !prec_labels.transfer_is_initialized(jpp.sp_idx)
                        || ! valid_stop_points[jpp.sp_idx.val]  ) {
                        continue;
                    }

                    /// the time at which we arrive at stop point jpp.sp_idx (using at most count-1 transfers)
                    //  hence we can board any vehicle arriving after previous_dt
                    const DateTime previous_dt = prec_labels.dt_transfer(jpp.sp_idx);
                    const DateTime previous_walking_duration = prec_labels.walking_duration_transfer(jpp.sp_idx);


                    /// we are at stop point jpp.idx at time previous_dt
                    /// waiting for the next vehicle journey of the journey_pattern jpp.jp_idx to embark on
                    /// the next vehicle journey will arrive at
                    ///   tmp_st_dt.second
                    /// the corresponding StopTime is
                    ///    tmp_st_dt.first
                    const auto tmp_st_dt =
                        next_st->next_stop_time(visitor.stop_event(), jpp.idx, previous_dt, visitor.clockwise());

                    /// if there is no vehicle arriving after previous_dt, nothing to do
                    if (tmp_st_dt.first == nullptr) {
                        continue;
                    }



                    bool update_boarding_stop_point = !is_onboard 
                                                        || visitor.comp(tmp_st_dt.second, workingDt)
                                                        ||  (tmp_st_dt.second ==  workingDt && previous_walking_duration < working_walking_duration) ;
                    // if(working_walking_duration != DateTimeUtils::not_valid) {
                    //     update_boarding_stop_point = update_boarding_stop_point 
                    //                                   || ( visitor.equal(previous_dt, base_dt, *it_st)
                    //                                         && previous_walking_duration < working_walking_duration 
                    //                                      );
                    // }



                    if (update_boarding_stop_point
                        ) {
                        /// we are at stop point jpp.idx at time previous_dt
                        /// waiting for the next vehicle journey of the journey_pattern jpp.jp_idx to embark on
                        /// the next vehicle journey will arrive at
                        ///   tmp_st_dt.second
                        /// the corresponding StopTime is
                        ///    tmp_st_dt.first
                        // const auto tmp_st_dt =
                        //     next_st->next_stop_time(visitor.stop_event(), jpp.idx, previous_dt, visitor.clockwise());
                        if (tmp_st_dt.first != nullptr) {

                            if (!is_onboard || &*it_st != tmp_st_dt.first) {
                                // st_range is quite cache
                                // unfriendly, so avoid using it if
                                // not really needed.
                                it_st = visitor.st_range(*tmp_st_dt.first).begin();
                                is_onboard = true;
                                l_zone = it_st->local_traffic_zone;
                                // note that if we have found a better
                                // pickup, and that this pickup does
                                // not have the same local traffic
                                // zone, we may miss some interesting
                                // solutions.
                            } else if (l_zone != it_st->local_traffic_zone) {
                                // if we can pick up in this vj with 2
                                // different zones, we can drop off
                                // anywhere (we'll chose later at
                                // which stop we pickup)
                                l_zone = std::numeric_limits<uint16_t>::max();
                            }

                            if(boarding_stop_point == SpIdx()) {
                                LOG4CPLUS_TRACE(logger, "Setting boarding stop point : " << count 
                                                << " to "  << data.pt_data->stop_points[jpp.sp_idx.val]->uri
                                                << " walk after : " << previous_walking_duration
                                        );
                            }
                            else {

                                LOG4CPLUS_TRACE(logger, "Switching boarding stop point : " << count 
                                                    << " from " << data.pt_data->stop_points[boarding_stop_point.val]->uri
                                                    << " to "  << data.pt_data->stop_points[jpp.sp_idx.val]->uri
                                                    << " working dt before : " << workingDt
                                                    << " working dt after : " << tmp_st_dt.second
                                                    << " walk before : " << working_walking_duration
                                                    << " walk after : " << previous_walking_duration
                                            );
                                LOG4CPLUS_TRACE(logger, "  previous_dt : : " << previous_dt 
                                    << " section_end(base_dt)" << it_st->section_end(base_dt, !visitor.clockwise())
                            );

                            }
                            workingDt = tmp_st_dt.second;
                            working_walking_duration = previous_walking_duration;
                            boarding_stop_point = jpp.sp_idx;

                            base_dt = tmp_st_dt.first->base_dt(workingDt, visitor.clockwise());
                            BOOST_ASSERT(!visitor.comp(workingDt, previous_dt));
                        }
                    }
                }
                if (is_onboard) {
                    const type::VehicleJourney* vj_stay_in = visitor.get_extension_vj(it_st->vehicle_journey);
                    if (vj_stay_in) {
                        bool applied = apply_vj_extension(visitor, rt_level, vj_stay_in, l_zone, base_dt, working_walking_duration, boarding_stop_point);
                        continue_algorithm = continue_algorithm || applied;
                    }
                }
            }
            /// mark the journey_pattern as visited, no need to explore it in the next round
            q_elt.second = visitor.init_queue_item();
        }
        continue_algorithm = continue_algorithm && this->foot_path(visitor);
    }
}

void RAPTOR::boucleRAPTOR(const bool clockwise, const nt::RTLevel rt_level, uint32_t max_transfers) {
    if (clockwise) {
        raptor_loop(raptor_visitor(), rt_level, max_transfers);
    } else {
        raptor_loop(raptor_reverse_visitor(), rt_level, max_transfers);
    }
}

std::vector<Path> RAPTOR::compute(const type::StopArea* departure,
                                  const type::StopArea* destination,
                                  int departure_hour,
                                  int departure_day,
                                  DateTime borne,
                                  const nt::RTLevel rt_level,
                                  const navitia::time_duration& transfer_penalty,
                                  bool clockwise,
                                  const type::AccessibiliteParams& accessibilite_params,
                                  uint32_t max_transfers,
                                  const std::vector<std::string>& forbidden_uri,
                                  const boost::optional<navitia::time_duration>& direct_path_dur) {
    map_stop_point_duration departures, destinations;

    for (const type::StopPoint* sp : departure->stop_point_list) {
        departures[SpIdx(*sp)] = {};
    }

    for (const type::StopPoint* sp : destination->stop_point_list) {
        destinations[SpIdx(*sp)] = {};
    }

    return compute_all(departures, destinations, DateTimeUtils::set(departure_day, departure_hour), rt_level,
                       transfer_penalty, borne, max_transfers, accessibilite_params, forbidden_uri, {}, clockwise,
                       direct_path_dur);
}

int RAPTOR::best_round(SpIdx sp_idx) {
    for (size_t i = 0; i <= this->count; ++i) {
        if (labels[i].dt_pt(sp_idx) == best_labels_pts[sp_idx]) {
            return i;
        }
    }
    return -1;
}


std::string RAPTOR::print_jpps_from_sp() {
    std::ostringstream output;
    for (auto stop_point_jpps : jpps_from_sp) {
        SpIdx sp_idx = stop_point_jpps.first;
        navitia::type::StopPoint* stop_point = data.pt_data->stop_points[sp_idx.val];
        output << "stop point : " << stop_point->uri << std::endl;
        for(auto jpp : stop_point_jpps.second) {
            // JppIdx jpp_idx = jpp.idx;
            // JpIdx jp_idx   = jpp.jp_idx;

            // const JourneyPatternPoint journey_pattern_point = data.dataRaptor->jp_container.get(jpp_idx);
            // const JourneyPattern      journey_pattern       = data.dataRaptor->jp_container.get(jp_idx);
            

            output << "  " << jpp.idx.val << " " << jpp.jp_idx.val << std::endl;
        }
    }
    return output.str();
}

std::string RAPTOR::print_current_labels() {
    std::ostringstream output;
    Labels& current_labels = labels[count];
    output << "count : " << count << std::endl;
    for (auto stop_point_date_itr : current_labels.get_dt_pts()) {
        SpIdx sp_idx = stop_point_date_itr.first;
        DateTime dt  = stop_point_date_itr.second;
        navitia::type::StopPoint* stop_point = data.pt_data->stop_points[sp_idx.val];
        output << "stop point : " << stop_point->uri;
        if ( current_labels.pt_is_initialized(sp_idx) ) {
            output << " dt_pt : " << str(dt) << std::endl;
        }
        else {
            output << " dt_pt : " << "not init" << std::endl;
        }
               
    }
    return output.str();
}

std::string RAPTOR::print_all_labels() {
    std::ostringstream output;
    for(uint32_t stop_point_id = 0; stop_point_id < data.pt_data->stop_points.size(); ++stop_point_id) {
        SpIdx sp_idx = SpIdx(stop_point_id);
        navitia::type::StopPoint* stop_point = data.pt_data->stop_points[sp_idx.val];

        bool print_stop_point = false;
        for(unsigned int count_id = 0; count_id < labels.size(); ++count_id) {
            Labels& current_labels = labels[count_id];
            if( current_labels.pt_is_initialized(sp_idx) || current_labels.transfer_is_initialized(sp_idx) ) {
                print_stop_point = true;
                break;
            }
        }

        if(print_stop_point) {
            output << "" << stop_point->uri << " : " << std::endl;

            for(unsigned int count_id = 0; count_id < labels.size(); ++count_id) {
                Labels& current_labels = labels[count_id];
                if( ! current_labels.pt_is_initialized(sp_idx) && !current_labels.transfer_is_initialized(sp_idx) ) {
                    continue;
                }
                output << "   count : " << count_id;
                output << " dt_pt : ";

                if ( current_labels.pt_is_initialized(sp_idx) ) {
                output <<  iso_string(current_labels.dt_pt(sp_idx), data);
                }
                else {
                    output <<  "not init";
                }

                output << " transfer_dt : ";

                if ( current_labels.transfer_is_initialized(sp_idx) ) {
                    output <<  iso_string(current_labels.dt_transfer(sp_idx), data);
                }
                else {
                    output <<  "not init";
                }    

                output << std::endl;        

            }
        }
    }
    return output.str();
}



std::string RAPTOR::print_best_labels() {
    std::ostringstream output;
    for(uint32_t stop_point_id = 0; stop_point_id < data.pt_data->stop_points.size(); ++stop_point_id) {
        SpIdx sp_idx = SpIdx(stop_point_id);
        navitia::type::StopPoint* stop_point = data.pt_data->stop_points[sp_idx.val];

        output << "" << stop_point->uri << " : " 
                << "best arrival : " << iso_string(best_labels_pts[sp_idx], data)
                << " best departure : " << iso_string(best_labels_transfers[sp_idx], data)
                << std::endl;

        
    }
    return output.str();
}

std::string RAPTOR::print_starting_points_snd_phase(std::vector<StartingPointSndPhase> & starting_points) {
    std::ostringstream output;
    for(auto start_point : starting_points) {
        navitia::type::StopPoint* stop_point = data.pt_data->stop_points[start_point.sp_idx.val];
        output << "" << stop_point->uri 
                << " count : " << start_point.count
                << " end_dt : " << iso_string(start_point.end_dt, data)
                << " fallback_dur : " << start_point.fallback_dur
                << " has_priority : " << start_point.has_priority
                << std::endl;
    }
    return output.str();
}

}  // namespace routing
}  // namespace navitia
