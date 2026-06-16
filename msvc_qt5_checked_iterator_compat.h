#ifndef MSVC_QT5_CHECKED_ITERATOR_COMPAT_H
#define MSVC_QT5_CHECKED_ITERATOR_COMPAT_H

#if defined(_MSC_VER) && _MSC_VER >= 1950
#include <cstddef>

namespace stdext {

template <typename T>
inline T *make_checked_array_iterator(T *ptr, std::size_t)
{
    return ptr;
}

template <typename T>
inline const T *make_checked_array_iterator(const T *ptr, std::size_t)
{
    return ptr;
}

template <typename T>
inline T *make_unchecked_array_iterator(T *ptr)
{
    return ptr;
}

template <typename T>
inline const T *make_unchecked_array_iterator(const T *ptr)
{
    return ptr;
}

} // namespace stdext
#endif

#endif // MSVC_QT5_CHECKED_ITERATOR_COMPAT_H
