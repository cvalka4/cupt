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
#ifndef CUPT_CACHE_BINARYVERSION_SEEN
#define CUPT_CACHE_BINARYVERSION_SEEN

#include <cupt/hashsums.hpp>
#include <cupt/cache/version.hpp>
#include <cupt/cache/relation.hpp>

namespace cupt {
namespace cache {

struct BinaryVersion: public Version
{
	struct RelationTypes
	{
		enum Type { PreDepends, Depends, Recommends, Suggests, Enhances, Conflicts, Breaks, Replaces, Count };
		static const string strings[];
		static const char* rawStrings[];
	};
	string architecture;
	uint32_t installedSize;
	string sourcePackageName;
	string sourceVersionString;
	bool essential;
	RelationLine relations[RelationTypes::Count];
	vector< string > provides;
	string shortDescription;
	string longDescription;
	string task; // do we need a special field for it?
	string tags;
	FileRecord file;

	bool isInstalled() const;
	virtual bool areHashesEqual(const shared_ptr< const Version >& other) const;

	static shared_ptr< BinaryVersion > parseFromFile(const Version::InitializationParameters&);
};

} // namespace
} // namespace

#endif
