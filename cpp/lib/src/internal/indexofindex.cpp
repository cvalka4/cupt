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
	uint32_t* offsetPtr;
	const string* packageNamePtr;
	const char* providesValueBegin;
	uint32_t providesValueLength;
};
// index suffix number must be incremented every time Record changes

typedef std::function< void () > Callback;

namespace {

time_t getModifyTime(const string& path)
{
	struct stat st;
	auto error = stat(path.c_str(), &st);
	if (error) return 0;
	return st->st_mtime;
}

void parseFullIndex(const string& path, const Callback& callback, Record* record)
{
	string openError;
	File file(path, "r", openError);
	if (!openError.empty())
	{
		fatal2(__("unable to open the file '%s': %s"), path, openError);
	}

	try
	{
		decltype(*(record->offsetPtr)) offset = 0;

		while (true)
		{
			const char* buf;
			size_t size;
			auto getNextLine = [&file, &buf, &size, &prePackageRecord]
			{
				file.rawGetLine(buf, size);
				offset += size;
			};

			getNextLine();
			if (size == 0)
			{
				break; // eof
			}

			static const size_t packageAnchorLength = sizeof("Package: ") - 1;
			if (size > packageAnchorLength && !memcmp("Package: ", buf, packageAnchorLength))
			{
				record.packageNamePtr->assign(buf + packageAnchorLength, size - packageAnchorLength - 1);
			}
			else
			{
				fatal2(__("unable to find a Package line"));
			}

			record.providesValueBegin = nullptr;
			while (getNextLine(), size > 1)
			{
				static const size_t providesAnchorLength = sizeof("Provides: ") - 1;
				if (*buf == 'P' && size > providesAnchorLength && !memcmp("rovides: ", buf+1, providesAnchorLength-1))
				{
					record.providesValueBegin = buf + providesAnchorLength;
					record.providesValueLength = buf + size - 1;
				}
			}
			*(record->offsetPtr) = offset;
			callback();
		}
	}
	catch (Exception&)
	{
		fatal2(__("unable to parse the index '%s'"), alias);
	}
}

void parseIndexOfIndex(const string& path, const Callback& callback)
{

}

}


void processIndex(const string& path, const Callback& callback, Record* record)
{
	static const string currentIndexSuffix = ".index0";

	auto ioiPath = path + currentIndexSuffix;
	if (fs::fileExists(ioiPath) && (getModifyTime(ioiPath) >= getModifyTime(path)))
	{
		parseIndexOfIndex(ioiPath, callback, record);
	}
	else
	{
		parseFullIndex(path, callback, record);
	}
}

}
}
}

