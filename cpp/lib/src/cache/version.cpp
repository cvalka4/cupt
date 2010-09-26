/**************************************************************************
*   Copyright (C) 2010 by Eugene V. Lyubimkin                             *
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

#include <set>

#include <cupt/cache/version.hpp>
#include <cupt/cache/releaseinfo.hpp>

namespace cupt {
namespace cache {

using std::set;

bool Version::parseRelations = true;
bool Version::parseInfoOnly = true;
bool Version::parseOthers = false;

Version::Version()
	: others(NULL)
{}

bool Version::isVerified() const
{
	size_t availableAsCount = availableAs.size();
	for (size_t i = 0; i < availableAsCount; ++i)
	{
		if (availableAs[i].release->verified)
		{
			return true;
		}
	}

	return false;
}

bool Version::operator<(const Version& other) const
{
	auto comparePackageNamesResult = packageName.compare(other.packageName);
	if (comparePackageNamesResult < 0)
	{
		return true;
	}
	else if (comparePackageNamesResult > 0)
	{
		return false;
	}
	return (versionString < other.versionString);
}

bool Version::operator==(const Version& other) const
{
	return packageName == other.packageName && versionString == other.versionString;
}

vector< Version::DownloadRecord > Version::getDownloadInfo() const
{
	set< string > seenFullDirs;

	vector< DownloadRecord > result;

	FORIT(availableAsRecordIt, availableAs)
	{
		const string& baseUri = availableAsRecordIt->release->baseUri;
		if (!baseUri.empty())
		{
			DownloadRecord record;
			record.directory = availableAsRecordIt->directory;
			record.baseUri = baseUri;

			string fullDir = baseUri + "/" + record.directory;
			if (seenFullDirs.insert(fullDir).second)
			{
				// new element
				result.push_back(std::move(record));
			}
		}
	}

	return result;
}

const string Version::Priorities::strings[] = {
	__("required"), __("important"), __("standard"), __("optional"), __("extra")
};

Version::~Version()
{
	if (others)
	{
		delete others;
	}
}

}
}


