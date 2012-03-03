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
#include <cupt/regex.hpp>

typedef FunctionSelector FS;

namespace {

class VersionSetGetter
{
	bool __binary; // source if false
	const Cache& __cache;
	mutable FS::Result* __cached_all_versions;

	vector< string > __get_package_names() const
	{
		return __binary ? __cache.getBinaryPackageNames() : __cache.getSourcePackageNames();
	}
	shared_ptr< Package > __get_package(const string& packageName) const
	{
		return __binary ? __cache.getBinaryPackage(packageName) : __cache.getSourcePackage(packageName);
	}
 public:
	explicit AllVersionGetter(const Cache& cache, bool binary)
		: __binary(binary), __cache(cache), __cached_all_versions(NULL)
	{}
	const FS::Result& getAll() const
	{
		if (!__cached_all_versions)
		{
			__cached_result = new FS::Result;
			for (const string& packageName: __get_package_names())
			{
				for (auto&& version: __get_package(packageName)->getVersions())
				{
					__cached_result->emplace_back(std::move(version));
				}
			}
		}
		return *__cached_result;
	}
	FS::Result get(const sregex& regex) const
	{
		smatch m;
		FS::Result result;
		for (const string& packageName: __get_package_names())
		{
			if (!regex_match(packageName, m, regex))
			{
				continue;
			}
			for (auto&& version: __get_package(packageName)->getVersions())
			{
				result.emplace_back(std::move(version));
			}
		}
		return result;
	}
	~AllVersionGetter()
	{
		delete __cache_result;
	}
};

class VersionSet
{
	const AllVersionGetter* __getter;
	bool __filtered;
	FS::Result __versions;

 public:
	explicit VersionSet(const AllVersionGetter* getter)
		: __getter(getter), __filtered(false)
	{}
	VersionSet(FS::Result&& versions)
		: __filtered(true), __versions(std::move(versions))
	{}
	FS::Result get()
	{
		if (__filtered)
		{
			return std::move(__versions);
		}
		else
		{
			return __getter->getAll();
		}
	}
	FS::Result get(const sregex& regex)
	{
		if (__filtered)
		{
			smatch m;
			__versions.remove_if([&regex, &m](const shared_ptr< const Version >& version)
			{
				return !regex_match(version->packageName, m, regex);
			});
			return std::move(__versions);
		}
		else
		{
			return __getter->get(regex);
		}
	}
}

class CommonFS: public FS
{
 public:
	typedef list< string > Arguments;
	virtual FS::Result select(const Cache& cache, bool filteredAlready, FS::Result&& from);
};

class AlgeFS: public CommonFS
{
 protected:
	list< unique< CommonFS > > _leaves;
 public:
	// postcondition: _leaves are not empty
	AlgeFS(const Arguments& arguments)
	{
		if (arguments.empty())
		{
			fatal2(__("the function should have at least one argument"));
		}
		for (const auto& argument: arguments)
		{
			_leaves.push_back(parseFunctionQuery(argument));
		}
	}
};

class AndFS: public AlgeFS
{
	AndFS(const Arguments& arguments)
		: AlgeFS(arguments)
	{}
	FS::Result select(const Cache&, FS::Result&& from)
	{

	}
};

}

unique_ptr< FS > parseFunctionQuery(const string& query)
{

}
