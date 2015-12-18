# encoding: utf-8

#  Copyright (c) 2001-2015, Canal TP and/or its affiliates. All rights reserved.
#
# This file is part of Navitia,
#     the software to build cool stuff with public transport.
#
# Hope you'll enjoy and contribute to this project,
#     powered by Canal TP (www.canaltp.fr).
# Help us simplify mobility and open public transport:
#     a non ending quest to the responsive locomotion way of traveling!
#
# LICENCE: This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.
#
# Stay tuned using
# twitter @navitia
# IRC #navitia on freenode
# https://groups.google.com/d/forum/navitia
# www.navitia.io

from functools import wraps
from jormungandr import authentication
import flask_restful
from datetime import datetime

def quota_control(func):
    """
    Decorator for quota control on every request
    """
    @wraps(func)
    def wrapper(*args, **kwargs):
        user = authentication.get_user(token=authentication.get_token())
        if user.block_until and datetime.utcnow() <= user.block_until:
            flask_restful.abort(429, message="Quota limit reached, please contact provider if you want to upgrade your current billing plan")

        return func(*args, **kwargs)

    return wrapper