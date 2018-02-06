/*
 * This file is protected by Copyright. Please refer to the COPYRIGHT file
 * distributed with this source distribution.
 *
 * This file is part of REDHAWK core.
 *
 * REDHAWK core is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * REDHAWK core is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 */

#ifndef REDHAWK_SHM_ALLOCATOR_H
#define REDHAWK_SHM_ALLOCATOR_H

#include <cstddef>
#include <string>

#include "ProcessHeap.h"

namespace redhawk {

    namespace shm {

        template <class T>
        struct Allocator : public std::allocator<T>
        {
        public:
            typedef std::allocator<T> base;
            typedef typename base::pointer pointer;
            typedef typename base::value_type value_type;
            typedef typename base::size_type size_type;

            template <typename U>
            struct rebind {
                typedef Allocator<U> other;
            };

            Allocator() throw() :
                base()
            {
            }

            Allocator(const Allocator& other) throw() :
                base(other)
            {
            }

            template <typename U>
            Allocator(const Allocator<U>& other) throw() :
                base(other)
            {
            }

            pointer allocate(size_type count)
            {
                size_type bytes = count * sizeof(value_type);
                return static_cast<pointer>(ProcessHeap::Instance().allocate(bytes));
            }

            void deallocate(pointer ptr, size_type /*unused*/)
            {
                ProcessHeap::Instance().deallocate(ptr);
            }
        };
    }
}

#endif // REDHAWK_SHM_ALLOCATOR_H