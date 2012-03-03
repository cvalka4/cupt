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

typedef shared_ptr< const Version >& SPCV;

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
			__versions.remove_if([&regex, &m](const SPCV& version)
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
	virtual FS::Result select(VersionSet&& from) = 0;
};

void __require_n_arguments(const CommonFS::Arguments& arguments, size_t n)
{
	if (arguments.size() != n)
	{
		fatal2(__("the function requires exactly '%zu' arguments"), n);
	}
}

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
	FS::Result select(VersionSet&& from)
	{
		auto result = __leaves.front().select(std::move(from));
		for (auto it = __leaves.begin() + 1; it != __leaves.end(); ++it)
		{
			result = it->select(std::move(result));
		}
		return result;
	}
};

class PredicateFS: public CommonFS
{
 protected:
	virtual bool _match(const SPCV& version) = 0;
 public:
	FS::Result select(VersionSet&& from)
	{
		FS::Result result = from.get();
		result.remove_if([this](const SPCV& version) { return !this->_match(version); });
		return result;
	}
}

sregex __parse_regex(const string& input)
{
	try
	{
		return sregex::compile(input, regex_constants::optimize);
	}
	catch (regex_error&)
	{
		fatal2(__("regular expression '%s' is not valid"), input);
	}
}

class RegexMatcher
{
	sregex __regex;
	smatch __m;
 public:
	RegexMatcher(const Arguments& arguments)
		: __regex(__require_n_arguments(arguments, 1), __get_regex(arguments[0]))
	{}
 protected:
	bool match(const string& input)
	{
		return regex_match(input, __m, __regex);
	}
};

class RegexMatchFS: public PredicateFS
{
	RegexMatcher __regex_matcher;
	std::function< string (const SPCV&) > __get_attribute;
 public:
	PackageNameFS(decltype(__get_attribute) getAttribute, const Arguments& arguments)
		: __get_attribute(getAttribute), __regex_matcher(arguments)
	{}
 protected:
	bool _match(const SPCV& version)
	{
		return __regex_matcher.match(__get_attribute(version));
	}
};

class PackageNameFS: public PredicateFS
{
	RegexMatcher __regex_matcher;
 public:
	PackageNameFS(const Arguments& arguments)
		: __regex_matcher(arguments)
	{}
 protected:
	bool _match(const SPCV& version)
	{
		return __regex_matcher.match(version->packageName);
	}
};

class PriorityFS: public PredicateFS
{
	RegexMatcher __regex_matcher;
 public:
	PriorityFS(const Arguments& arguments)
		: __regex_matcher(arguments)
	{}
 protected:
	bool _match(const shared_ptr< const Version >& version)
	{
		return __regex_matches.match(version->priority);
	}
}

constructFSByName(const string& functionName, const CommonFS::Arguments& arguments)
{
	#define CONSTRUCT_FS(name, code) if (functionName == name) { return code; }
	CONSTRUCT_FS("and", AndFS(arguments))
	CONSTRUCT_FS("package", RegexMatchFS([](const SPCV& version) { return version->packageName; }, arguments))
	CONSTRUCT_FS("priority", RegexMatchFS([](const SPCV& version) { return version->priority; }, arguments))
	fatal2(__("unknown selector function '%s'"), functionName);
	__builtin_unreachable();
}

unique_ptr< FS > parseFunctionQuery(const string& query)
{

}
