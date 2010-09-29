cmake_minimum_required(VERSION 2.6)
project(Cupt)

IF(EXISTS /usr/bin/g++-4.5)
	set(CMAKE_CXX_COMPILER /usr/bin/g++-4.5)
ELSE(EXISTS /usr/bin/g++-4.5)
	message(FATAL_ERROR "g++ 4.5.0 or greater required")
ENDIF(EXISTS /usr/bin/g++-4.5)

IF(NOT EXISTS /usr/include/boost/xpressive/xpressive_dynamic.hpp)
	message(FATAL_ERROR "missing Boost.Xpressive library")
ENDIF(NOT EXISTS /usr/include/boost/xpressive/xpressive_dynamic.hpp)

IF(NOT EXISTS /usr/include/boost/program_options.hpp)
	message(FATAL_ERROR "missing Boost.ProgramOptions library")
ENDIF(NOT EXISTS /usr/include/boost/program_options.hpp)

find_package(Boost 1.42.0)
IF(Boost_FOUND)
	IF(Boost_VERSION LESS 104200)
		message(FATAL_ERROR "need Boost of version 1.42.0")
	ENDIF(Boost_VERSION LESS 104200)
ELSE(Boost_FOUND)
	message(FATAL_ERROR "missing Boost")
ENDIF(Boost_FOUND)

IF(NOT EXISTS /usr/include/readline/readline.h)
	message(FATAL_ERROR "missing GNU Readline library")
ENDIF(NOT EXISTS /usr/include/readline/readline.h)

set(CMAKE_CXX_FLAGS "-Wall -Wextra -std=gnu++0x  -fPIC")

OPTION(VERBOSE "is build verbose")
IF(VERBOSE)
	set(CMAKE_VERBOSE_MAKEFILE TRUE)
ENDIF(VERBOSE)

OPTION(LOCAL "is build local" ON)
IF(LOCAL)
	set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DCUPT_LOCAL_BUILD")
ENDIF(LOCAL)

OPTION(OPTIMIZE "is build optimized")
IF(OPTIMIZE GREATER 0)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DNDEBUG -O${OPTIMIZE}")
	IF(OPTIMIZE GREATER 1)
		# set(CMAKE_LD_FLAGS "${CMAKE_LD_FLAGS} -flto")
		# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto")
	ENDIF(OPTIMIZE GREATER 1)
ENDIF(OPTIMIZE GREATER 0)

OPTION(PROFILE "for profiling")
IF(PROFILE)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ggdb")
ENDIF(PROFILE)

MACRO(ADD_PRECOMPILED_HEADER Target)
	get_target_property(Sources ${Target} SOURCES)
	# message(STATUS "Sources found for ${Target}: ${Sources}")
	SET_SOURCE_FILES_PROPERTIES(${Sources}
		PROPERTIES
		COMPILE_FLAGS "-include ../precompiled/precompiled.hpp -Winvalid-pch"
		OBJECT_DEPENDS "precompiled"
	)
	add_dependencies(${Target} precompiled)
ENDMACRO(ADD_PRECOMPILED_HEADER)

set(CUPT_SOVERSION 0)

# detect version from debian/changelog
execute_process(
	COMMAND "dpkg-parsechangelog" "-l../debian/changelog"
	COMMAND "grep" "^Version"
	COMMAND "cut" "-d " "-f" "2"
	OUTPUT_VARIABLE CUPT_VERSION
	OUTPUT_STRIP_TRAILING_WHITESPACE
)

IF(CUPT_VERSION)
	message(STATUS "Detected Cupt version: ${CUPT_VERSION}")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DCUPT_VERSION=${CUPT_VERSION}")
ELSE(CUPT_VERSION)
	message(FATAL_ERROR "Unable to detect Cupt version.")
ENDIF(CUPT_VERSION)

add_subdirectory(console)
add_subdirectory(lib)
add_subdirectory(precompiled)
add_subdirectory(downloadmethods)
