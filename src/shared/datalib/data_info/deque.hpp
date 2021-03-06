/*
 *  Copyright (C) 2014 Denilson das Merc�s Amorim (aka LINK/2012)
 *  Licensed under the Boost Software License v1.0 (http://opensource.org/licenses/BSL-1.0)
 *
 */
#pragma once
#include <deque>
#include <datalib/data_info/dyncontainer.hpp>

namespace datalib {

template<typename... Args>
struct is_dyncontainer<std::deque<Args...>> : std::true_type
{};

template<typename... Args>
struct data_info<std::deque<Args...>> :
    data_info_dyncontainer<std::deque<Args...>, true>
{};

} // namespace datalib
