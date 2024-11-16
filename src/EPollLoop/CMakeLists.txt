# Copyright 2021-2024 Kevin Backhouse.
#
# This file is part of EPollLoop.
#
# EPollLoop is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# EPollLoop is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with EPollLoop.  If not, see <https://www.gnu.org/licenses/>.

add_library(EPollLoop SHARED)

target_sources(
        EPollLoop PRIVATE
        EPollLoop.cpp
        ../../include/EPollLoop/EPollLoop.hpp)

target_include_directories(EPollLoop PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/EPollLoop>
)

write_basic_package_version_file(EPollLoopConfigVersion.cmake COMPATIBILITY ExactVersion)

install(TARGETS EPollLoop EXPORT EPollLoopConfig
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(EXPORT EPollLoopConfig DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/EPollLoop")

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/EPollLoopConfigVersion.cmake
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/EPollLoop")
