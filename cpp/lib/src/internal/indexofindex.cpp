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
// for ntohl
#include <arpa/inet.h>

#include <cupt/file.hpp>

#include <internal/filesystem.hpp>

#include <internal/indexofindex.hpp>

namespace cupt {
namespace internal {
namespace ioi {

namespace {

time_t getModifyTime(const string& path)
{
	struct stat st;
	auto error = stat(path.c_str(), &st);
	if (error) return 0;
	return st.st_mtime;
}

void parseFullIndex(const string& path, const Callbacks& callbacks, Record* record)
{
	string openError;
	File file(path, "r", openError);
	if (!openError.empty())
	{
		fatal2(__("unable to open the file '%s': %s"), path, openError);
	}

	uint32_t offset = 0;

	while (true)
	{
		const char* buf;
		size_t size;
		auto getNextLine = [&file, &buf, &size, &offset]
		{
			file.rawGetLine(buf, size);
			offset += size;
		};

		getNextLine();
		if (size == 0)
		{
			break; // eof
		}
		*(record->offsetPtr) = offset;

		static const size_t packageAnchorLength = sizeof("Package: ") - 1;
		if (size > packageAnchorLength && !memcmp("Package: ", buf, packageAnchorLength))
		{
			record->packageNamePtr->assign(buf + packageAnchorLength, size - packageAnchorLength - 1);
		}
		else
		{
			fatal2(__("unable to find a Package line"));
		}
		callbacks.main();

		while (getNextLine(), size > 1)
		{
			static const size_t providesAnchorLength = sizeof("Provides: ") - 1;
			if (*buf == 'P' && size > providesAnchorLength && !memcmp("rovides: ", buf+1, providesAnchorLength-1))
			{
				callbacks.provides(buf + providesAnchorLength, buf + size - 1);
			}
		}
	}
}

namespace field {

const char provides = 'p';

}

void parseIndexOfIndex(const string& path, const Callbacks& callbacks, Record* record)
{
	string openError;
	File file(path, "r", openError);
	if (!openError.empty())
	{
		fatal2(__("unable to open the file '%s': %s"), path, openError);
	}

	const char* buf;
	size_t bufSize;
	while (file.rawGetLine(buf, bufSize), bufSize > 0)
	{
		{ // offset and package name:
			if (bufSize < 6)
			{
				fatal2i("ioi: offset and package name: too small line");
			}
			(*record->offsetPtr) = ntohl(*reinterpret_cast< const uint32_t* >(buf));
			record->packageNamePtr->assign(buf+sizeof(uint32_t), buf+bufSize-1);
			callbacks.main();
		}
		while (file.rawGetLine(buf, bufSize), bufSize > 1)
		{
			auto fieldType = buf[0];
			switch (fieldType)
			{
				case field::provides:
					callbacks.provides(buf+1, buf+bufSize-1);
					break;
				default:
					fatal2i("ioi: invalid field type %zu", size_t(fieldType));
			}
		}
	}
}

}


void processIndex(const string& path, const Callbacks& callbacks, Record* record)
{
	static const string currentIndexSuffix = ".index0";

	auto ioiPath = path + currentIndexSuffix;
	if (fs::fileExists(ioiPath) && (getModifyTime(ioiPath) >= getModifyTime(path)))
	{
		parseIndexOfIndex(ioiPath, callbacks, record);
	}
	else
	{
		parseFullIndex(path, callbacks, record);
	}
}

}
}
}

