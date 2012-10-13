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
#include "common.hpp"

#include <cupt/system/state.hpp>
#include <cupt/cache/binarypackage.hpp>

bool isPackageInstalled(const Cache& cache, const string& packageName)
{
	auto&& installedInfo = cache.getSystemState()->getInstalledInfo(packageName);
	return (installedInfo && installedInfo->status != system::State::InstalledRecord::Status::ConfigFiles);
}

typedef BinaryVersion::RelationTypes BRT;

ReverseDependsIndexType computeReverseDependsIndex(const Cache& cache,
		const vector< BRT::Type >& relationTypes)
{
	ReverseDependsIndexType reverseDependsIndex;
	for (const string& packageName: cache.getBinaryPackageNames())
	{
		set< string > usedKeys;

		auto package = cache.getBinaryPackage(packageName);
		for (const auto& version: *package)
		{
			for (auto relationGroup: relationTypes)
			{
				const RelationLine& relationLine = version->relations[relationGroup];
				for (const auto& relationExpression: relationLine)
				{
					auto satisfyingVersions = cache.getSatisfyingVersions(relationExpression);
					for (const auto& satisfyingVersion: satisfyingVersions)
					{
						const string& satisfyingPackageName = satisfyingVersion->packageName;
						if (usedKeys.insert(satisfyingPackageName).second)
						{
							reverseDependsIndex[satisfyingPackageName].push_back(package);
						}
					}
				}
			}
		}
	}
	return reverseDependsIndex;
}

void foreachReverseDependency(const Cache& cache, const ReverseDependsIndexType& index,
		const BinaryVersion* version, BRT::Type relationType,
		const std::function< void (const BinaryVersion*, const RelationExpression&) > callback)
{
	auto packageCandidatesIt = index.find(version->packageName);
	if (packageCandidatesIt != index.end())
	{
		const auto& packageCandidates = packageCandidatesIt->second;
		for (auto packageCandidate: packageCandidates)
		{
			for (const auto& candidateVersion: *packageCandidate)
			{
				for (const auto& relationExpression: candidateVersion->relations[relationType])
				{
					auto satisfyingVersions = cache.getSatisfyingVersions(relationExpression);
					for (const auto& satisfyingVersion: satisfyingVersions)
					{
						if (satisfyingVersion == version)
						{
							callback(candidateVersion, relationExpression);
							goto candidate;
						}
					}
				}
				candidate:
				;
			}
		}
	}
}

