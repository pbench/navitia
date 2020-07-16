/* Copyright © 2001-2015, Canal TP and/or its affiliates. All rights reserved.

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

#pragma once

#include "journey.h"
#include "raptor_utils.h"
#include "utils/multi_obj_pool.h"
#include "routing.h"

namespace navitia {
namespace routing {

struct RAPTOR;
struct StartingPointSndPhase;

// this structure compare 2 solutions.  It chooses which solutions
// will be kept at the end (only non dominated solutions will be kept
// by the pool).
struct Dominates {
    bool request_clockwise;
    Dominates(bool rc) : request_clockwise(rc) {}
    bool operator()(const Journey& lhs, const Journey& rhs) const {
        return lhs.better_on_dt(rhs, request_clockwise) && lhs.better_on_transfer(rhs) && lhs.better_on_sn(rhs);
    }
};

struct ParetoFrontLogger {
    log4cplus::Logger logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("raptor"));

    void at_dominated(const Journey& to_insert, const Journey& in_pool) {
        LOG4CPLUS_DEBUG(logger, "Journey dominated ! " << std::endl
                                                       << to_insert << "  dominated by : " << std::endl
                                                       << in_pool);
    }
    void at_dominates(const Journey& /*to_insert*/, const Journey& in_pool) {
        LOG4CPLUS_DEBUG(logger, "Journey removed from solution pool " << std::endl << in_pool);
    }
    void at_inserted(const Journey& j) {
        LOG4CPLUS_DEBUG(logger, "Adding  journey to solution pool" << std::endl << j);
    }
};

typedef ParetoFront<Journey, Dominates, ParetoFrontLogger> Solutions;

// deps (resp. arrs) are departure (resp. arrival) stop points and
// durations (not clockwise dependent).
void read_solutions(const RAPTOR& raptor,
                    Solutions& solutions,  // all raptor solutions, modified by side effects
                    const bool clockwise,
                    const DateTime& departure_datetime,
                    const routing::map_stop_point_duration& deps,
                    const routing::map_stop_point_duration& arrs,
                    const type::RTLevel rt_level,
                    const type::AccessibiliteParams& accessibilite_params,
                    const navitia::time_duration& transfer_penalty,
                    const StartingPointSndPhase& end_point);

Path make_path(const Journey& journey, const type::Data& data);

}  // namespace routing
}  // namespace navitia
