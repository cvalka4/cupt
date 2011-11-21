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
#include <internal/regex.hpp>
#include <internal/cachefiles.hpp>
#include <internal/filesystem.hpp>

// TODO/API break/: remove deprecated entities

namespace cupt {

typedef internal::CacheImpl::PrePackageRecord PrePackageRecord;

Cache::Cache(shared_ptr< const Config > config, bool useSource, bool useBinary, bool useInstalled,
		const vector< string >& packageNameGlobsToReinstall)
{
	__impl = new internal::CacheImpl;
	__impl->config = config;
	__impl->binaryArchitecture.reset(new string(config->getString("apt::architecture")));

	FORIT(it, packageNameGlobsToReinstall)
	{
		__impl->packageNameRegexesToReinstall.push_back(internal::globToRegex(*it));
	}

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

string Cache::getPathOfIndexList(const IndexEntry& entry) const
{
	return internal::cachefiles::getPathOfIndexList(*__impl->config, entry);
}

string Cache::getPathOfReleaseList(const IndexEntry& entry) const
{
	return internal::cachefiles::getPathOfReleaseList(*__impl->config, entry);
}

string Cache::getPathOfExtendedStates() const
{
	return internal::cachefiles::getPathOfExtendedStates(*__impl->config);
}

string Cache::getDownloadUriOfReleaseList(const IndexEntry& entry) const
{
	return internal::cachefiles::getDownloadUriOfReleaseList(entry);
}

vector< Cache::IndexDownloadRecord > Cache::getDownloadInfoOfIndexList(const IndexEntry& entry) const
{
	return internal::cachefiles::getDownloadInfoOfIndexList(*__impl->config, entry);
}

vector< Cache::LocalizationDownloadRecord > Cache::getDownloadInfoOfLocalizedDescriptions(const IndexEntry& entry) const
{
	return internal::cachefiles::getDownloadInfoOfLocalizedDescriptions(*__impl->config, entry);
}

vector< string > Cache::getBinaryPackageNames() const
{
	vector< string > result;
	FORIT(it, __impl->preBinaryPackages)
	{
		result.push_back(it->first);
	}
	return result;
}

vector< string > Cache::getSourcePackageNames() const
{
	vector< string > result;
	FORIT(it, __impl->preSourcePackages)
	{
		result.push_back(it->first);
	}
	return result;
}

shared_ptr< const BinaryPackage > Cache::getBinaryPackage(const string& packageName) const
{
	return __impl->getBinaryPackage(packageName);
}

shared_ptr< const SourcePackage > Cache::getSourcePackage(const string& packageName) const
{
	return __impl->getSourcePackage(packageName);
}

ssize_t Cache::getPin(const shared_ptr< const Version >& version) const
{
	string installedVersionString;
	if (dynamic_pointer_cast< const BinaryVersion >(version))
	{
		auto package = getBinaryPackage(version->packageName);
		if (package)
		{
			auto installedVersion = package->getInstalledVersion();
			if (installedVersion)
			{
				installedVersionString = installedVersion->versionString;
			}
		}
	}

	return __impl->getPin(version, installedVersionString);
}

vector< Cache::PinnedVersion > Cache::getSortedPinnedVersions(const shared_ptr< const Package >& package) const
{
	vector< Cache::PinnedVersion > result;

	auto versions = package->getVersions();

	string installedVersionString;
	if (auto binaryPackage = dynamic_pointer_cast< const BinaryPackage >(package))
	{
		auto installedVersion = binaryPackage->getInstalledVersion();
		if (installedVersion)
		{
			installedVersionString = installedVersion->versionString;
		}
	}

	size_t versionCount = versions.size();
	for (size_t i = 0; i < versionCount; ++i)
	{
		shared_ptr< const Version >& version = versions[i];
		result.push_back(PinnedVersion(version, __impl->getPin(version, installedVersionString)));
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

shared_ptr< const Version > Cache::getPolicyVersion(const shared_ptr< const Package >& package) const
{
	auto sortedPinnedVersions = getSortedPinnedVersions(package);
	// not assuming the package have at least valid version...
	if (sortedPinnedVersions.empty())
	{
		return shared_ptr< const Version >();
	}
	else
	{
		// so, just return version with maximum "candidatness"
		return sortedPinnedVersions[0].version;
	}
}

shared_ptr< const system::State > Cache::getSystemState() const
{
	return __impl->systemState;
}

bool Cache::isAutomaticallyInstalled(const string& packageName) const
{
	return __impl->extendedInfo.automaticallyInstalled.count(packageName);
}

vector< shared_ptr< const BinaryVersion > >
Cache::getSatisfyingVersions(const RelationExpression& relationExpression) const
{
	return __impl->getSatisfyingVersions(relationExpression);
}

vector< shared_ptr< const BinaryVersion > > Cache::getInstalledVersions() const
{
	vector< shared_ptr< const BinaryVersion > > result;

	auto packageNames = __impl->systemState->getInstalledPackageNames();
	FORIT(packageNameIt, packageNames)
	{
		const string& packageName = *packageNameIt;

		auto package = getBinaryPackage(packageName);
		if (!package)
		{
			fatal2("internal error: unable to find the package '%s'", packageName);
		}
		auto version = package->getInstalledVersion();
		if (!version)
		{
			fatal2("internal error: the package '%s' does not have installed version", packageName);
		}

		result.push_back(version);
	}

	return result;
}

const Cache::ExtendedInfo& Cache::getExtendedInfo() const
{
	return __impl->extendedInfo;
}

pair< string, string > Cache::getLocalizedDescriptions(const shared_ptr< const BinaryVersion >& version) const
{
	return __impl->getLocalizedDescriptions(version);
}

// static
bool Cache::verifySignature(const shared_ptr< const Config >& config, const string& path)
{
	return internal::cachefiles::verifySignature(*config, path);
}

string Cache::getPathOfCopyright(const shared_ptr< const BinaryVersion >& version)
{
	if (!version->isInstalled())
	{
		return string();
	}

	return string("/usr/share/doc/") + version->packageName + "/copyright";
}

string Cache::getPathOfChangelog(const shared_ptr< const BinaryVersion >& version)
{
	if (!version->isInstalled())
	{
		return string();
	}

	const string& packageName = version->packageName;
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

