/**************************************************************************
*   Copyright (C) 2012 by Eugene V. Lyubimkin                             *
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License                  *
*   (version 3 or above) as published by the Free Software Foundation.    *
*                                                                         *
*   This program is distributed in the hope that it will be useful,       *
*   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
*   GNU General Public License for more details.                          *
*                                                                         *
*   You should have received a copy of the GNU GPL                        *
*   along with this program; if not, write to the                         *
*   Free Software Foundation, Inc.,                                       *
*   51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA               *
**************************************************************************/
// for stat
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <internal/filesystem.hpp>

namespace cupt {
namespace internal {
namespace ioi {

struct Record
{
	const string* packageNamePtr;
	const string* providesValue;
}
typedef std::function< void (Record*) > Callback;

namespace {

time_t getModifyTime(const string& path)
{
	struct stat st;
	auto error = stat(path.c_str(), &st);
	if (error) return 0;
	return st->st_mtime;
}

void parseFullIndex(const string& path, const Callback& callback)
{

}

void parseIndexOfIndex(const string& path, const Callback& callback)
{

}

}


void processIndex(const string& path, const Callback& callback)
{
	static const string currentIndexSuffix = ".index0";

	auto ioiPath = path + currentIndexSuffix;
	if (fs::fileExists(ioiPath) && (getModifyTime(ioiPath) >= getModifyTime(path)))
	{
		parseIndexOfIndex(ioiPath, callback);
	}
	else
	{
		parseFullIndex(path, callback);
	}
}

}
}
}

