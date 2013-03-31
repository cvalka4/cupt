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

#include <algorithm>

#include <cupt/config.hpp>
#include <cupt/cache.hpp>
#include <cupt/cache/binarypackage.hpp>
#include <cupt/cache/binaryversion.hpp>
#include <cupt/system/state.hpp>

#include <internal/cacheimpl.hpp>
#include <internal/filesystem.hpp>

namespace cupt {

typedef internal::CacheImpl::PrePackageRecord PrePackageRecord;

Cache::Cache(shared_ptr< const Config > config, bool useSource, bool useBinary, bool useInstalled)
{
	__impl = new internal::CacheImpl;
	__impl->config = config;
	__impl->binaryArchitecture.reset(new string(config->getString("apt::architecture")));

	{ // ugly hack to copy trusted keyring from APT whenever possible, see #647001
		auto cuptKeyringPath = config->getString("gpgv::trustedkeyring");
		auto tempPath = cuptKeyringPath + ".new.temp";

		auto result = std::system(format2("rm -f %s &&"
				"(apt-key exportall | gpg --batch --no-default-keyring --keyring %s --import) >/dev/null 2>/dev/null &&"
				"chmod -f +r %s",
				tempPath, tempPath, tempPath).c_str());
		if (result == 0)
		{
			internal::fs::move(tempPath, cuptKeyringPath); // ignoring errors
		}
		unlink(tempPath.c_str()); // in case of system() or move() above failed
	}

	__impl->parseSourcesLists();

	if (useInstalled)
	{
		__impl->systemState.reset(new system::State(config, __impl));
	}

	__impl->processIndexEntries(useBinary, useSource);
	__impl->parsePreferences();
	__impl->parseExtendedStates();
}

Cache::~Cache()
{
	delete __impl;
}

vector< shared_ptr< const ReleaseInfo > > Cache::getBinaryReleaseData() const
{
	return __impl->binaryReleaseData;
}

vector< shared_ptr< const ReleaseInfo > > Cache::getSourceReleaseData() const
{
	return __impl->sourceReleaseData;
}

vector< Cache::IndexEntry > Cache::getIndexEntries() const
{
	return __impl->indexEntries;
}

vector< PackageId > Cache::getBinaryPackageIds() const
{
	vector< PackageId > result;
	FORIT(it, __impl->preBinaryPackages)
	{
		result.push_back(it->first);
	}
	return result;
}

vector< PackageId > Cache::getSourcePackageIds() const
{
	vector< PackageId > result;
	FORIT(it, __impl->preSourcePackages)
	{
		result.push_back(it->first);
	}
	return result;
}

const BinaryPackage* Cache::getBinaryPackage(PackageId packageId) const
{
	return __impl->getBinaryPackage(packageId);
}

const SourcePackage* Cache::getSourcePackage(PackageId packageId) const
{
	return __impl->getSourcePackage(packageId);
}

ssize_t Cache::getPin(const Version* version) const
{
	auto getBinaryPackageFromVersion = [this, &version]() -> const BinaryPackage*
	{
		if (dynamic_cast< const BinaryVersion* >(version))
		{
			return getBinaryPackage(version->packageId);
		}
		else
		{
			return nullptr;
		}
	};

	return __impl->getPin(version, getBinaryPackageFromVersion);
}

vector< Cache::PinnedVersion > Cache::getSortedPinnedVersions(const Package* package) const
{
	vector< Cache::PinnedVersion > result;

	auto getBinaryPackage = [&package]()
	{
		return dynamic_cast< const BinaryPackage* >(package);
	};
	for (const auto& version: *package)
	{
		result.push_back(PinnedVersion { version, __impl->getPin(version, getBinaryPackage) });
	}

	auto sorter = [](const PinnedVersion& left, const PinnedVersion& right) -> bool
	{
		if (left.pin < right.pin)
		{
			return false;
		}
		else if (left.pin > right.pin)
		{
			return true;
		}
		else
		{
			return compareVersionStrings(left.version->versionString, right.version->versionString) > 0;
		}
	};
	std::stable_sort(result.begin(), result.end(), sorter);

	return result;
}

const Version* Cache::getPreferredVersion(const Package* package) const
{
	auto sortedPinnedVersions = getSortedPinnedVersions(package);
	// not assuming the package have at least valid version...
	if (sortedPinnedVersions.empty())
	{
		return nullptr;
	}
	else
	{
		// so, just return version with maximum "candidatness"
		return sortedPinnedVersions[0].version;
	}
}

const system::State* Cache::getSystemState() const
{
	return __impl->systemState.get();
}

bool Cache::isAutomaticallyInstalled(PackageId packageId) const
{
	return __impl->extendedInfo.automaticallyInstalled.count(packageId);
}

vector< const BinaryVersion* >
Cache::getSatisfyingVersions(const RelationExpression& relationExpression) const
{
	return __impl->getSatisfyingVersions(relationExpression);
}

vector< const BinaryVersion* > Cache::getInstalledVersions() const
{
	vector< const BinaryVersion* > result;

	auto packageIds = __impl->systemState->getInstalledPackageIds();
	result.reserve(packageIds.size());
	for (auto packageId: packageIds)
	{
		auto package = getBinaryPackage(packageId);
		if (!package)
		{
			fatal2i("unable to find the package '%s'", packageId.name());
		}
		auto version = package->getInstalledVersion();
		if (!version)
		{
			fatal2i("the package '%s' does not have installed version", packageId.name());
		}

		result.push_back(std::move(version));
	}

	return result;
}

const Cache::ExtendedInfo& Cache::getExtendedInfo() const
{
	return __impl->extendedInfo;
}

string Cache::getLocalizedDescription(const BinaryVersion* version) const
{
	return __impl->getLocalizedDescription(version);
}

string Cache::getPathOfCopyright(const BinaryVersion* version)
{
	if (!version->isInstalled())
	{
		return string();
	}

	return string("/usr/share/doc/") + version->packageId.name() + "/copyright";
}

string Cache::getPathOfChangelog(const BinaryVersion* version)
{
	if (!version->isInstalled())
	{
		return string();
	}

	const string& packageName = version->packageId.name();
	const string commonPart = string("/usr/share/doc/") + packageName + "/";
	if (version->versionString.find('-') == string::npos)
	{
		return commonPart + "changelog.gz"; // non-native package
	}
	else
	{
		return commonPart + "changelog.Debian.gz"; // native package
	}
}

bool Cache::memoize = false;

} // namespace

