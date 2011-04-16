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

#include <cupt/cache/binarypackage.hpp>
#include <cupt/cache/binaryversion.hpp>

namespace cupt {
namespace cache {

BinaryPackage::BinaryPackage(const shared_ptr< const string >& binaryArchitecture, bool allowReinstall)
	: Package(binaryArchitecture), __allow_reinstall(allowReinstall)
{}

shared_ptr< Version > BinaryPackage::_parse_version(const Version::InitializationParameters& initParams) const
{
	auto version = BinaryVersion::parseFromFile(initParams);
	if (version->isInstalled() and __allow_reinstall)
	{
		version->versionString += "~installed";
	}
	return version;
}

bool BinaryPackage::_is_architecture_appropriate(const shared_ptr< const Version >& version) const
{
	auto binaryVersion = static_pointer_cast< const BinaryVersion >(version);
	if (binaryVersion->isInstalled())
	{
		return true;
	}
	auto architecture = binaryVersion->architecture;
	return (architecture == "all" || architecture == *_binary_architecture);
}

vector< shared_ptr< const BinaryVersion > > BinaryPackage::getVersions() const
{
	auto source = _get_versions();
	vector< shared_ptr< const BinaryVersion > > result;
	FORIT(it, source)
	{
		result.push_back(static_pointer_cast< const BinaryVersion >(*it));
	}
	return result;
}

shared_ptr< const BinaryVersion > BinaryPackage::getInstalledVersion() const
{
	auto source = getVersions();
	if (!source.empty() && source[0]->isInstalled())
	{
		// here we rely on the fact that installed version (if exists) adds first to the cache/package
		return source[0];
	}
	else
	{
		return shared_ptr< const BinaryVersion >();
	}
}

}
}

