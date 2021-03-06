/*
 *  Copyright (C) 2014 Denilson das Merc�s Amorim (aka LINK/2012)
 *  Licensed under the Boost Software License v1.0 (http://opensource.org/licenses/BSL-1.0)
 *
 */
#pragma once
#include <datalib/data_info.hpp>
#include <datalib/detail/optional.hpp>

#ifndef BOOST_OPTIONAL_NO_IOFWD
#error Please include optional<T> from datalib/io/optional.hpp
#endif

namespace datalib {


/*
 *  data_info<> specialization for 'optional<T>'
 */
template<typename T>
struct data_info<optional<T>> : data_info<T>
{
    static const char separator = '\0';

    // Performs cheap precomparision
    static bool precompare(const optional<T>& a, const optional<T>& b)
    {
        return ((bool)(a) == (bool)(b));
    }
};


} // namespace datalib

