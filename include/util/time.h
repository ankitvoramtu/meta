/**
 * @file time.h
 * @author Chase Geigle
 *
 * All files in META are released under the MIT license. For more details,
 * consult the file LICENSE in the root of the project.
 */

#ifndef META_UTIL_TIME_H_
#define META_UTIL_TIME_H_

#include <chrono>

namespace meta
{
namespace common
{

/**
 * Times a given function.
 * @param functor the function to be timed
 * @return the length of time, expressed as a Duration, the function
 *  took to run. Defaults to milliseconds.
 */
template <class Duration = std::chrono::milliseconds, class Functor>
Duration time(Functor&& functor)
{
    auto start = std::chrono::steady_clock::now();
    functor();
    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<Duration>(end - start);
}
}
}

#endif
