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
#include <common/regex.hpp>

#include <cupt/cache/binarypackage.hpp>
#include <cupt/cache/sourcepackage.hpp>

#include "functionselectors.hpp"

FunctionSelector::FunctionSelector()
{}

FunctionSelector::~FunctionSelector()
{}

namespace {

typedef FunctionSelector FS;
typedef shared_ptr< const Version > SPCV;
typedef list< SPCV > FSResult;

bool __spcv_less(const Cache& cache, const SPCV& left, const SPCV& right)
{
	if (left->packageName < right->packageName)
	{
		return true;
	}
	if (left->packageName > right->packageName)
	{
		return false;
	}
	auto leftPin = cache.getPin(left);
	auto rightPin = cache.getPin(right);
	if (leftPin < rightPin)
	{
		return true;
	}
	if (leftPin > rightPin)
	{
		return false;
	}
	return left->versionString < right->versionString;
}

class VersionSetGetter
{
	bool __binary; // source if false
	const Cache& __cache;
	mutable FSResult* __cached_all_versions;

	vector< string > __get_package_names() const
	{
		auto result = __binary ? __cache.getBinaryPackageNames() : __cache.getSourcePackageNames();
		std::sort(result.begin(), result.end());
		return result;
	}
	shared_ptr< const Package > __get_package(const string& packageName) const
	{
		return __binary ? shared_ptr< const Package >(__cache.getBinaryPackage(packageName))
				: shared_ptr< const Package >(__cache.getSourcePackage(packageName));
	}
	void __add_package_to_result(const string& packageName, FSResult* result) const
	{
		// we call getSortedPinnedVersions() to place versions of the same package in the preference order
		for (auto&& pinnedVersion: __cache.getSortedPinnedVersions(__get_package(packageName)))
		{
			result->emplace_back(std::move(pinnedVersion.version));
		}
	}
 public:
	explicit VersionSetGetter(const Cache& cache, bool binary)
		: __binary(binary), __cache(cache), __cached_all_versions(NULL)
	{}
	const FSResult& getAll() const
	{
		if (!__cached_all_versions)
		{
			__cached_all_versions = new FSResult;
			for (const string& packageName: __get_package_names())
			{
				__add_package_to_result(packageName, __cached_all_versions);
			}
		}
		return *__cached_all_versions;
	}
	FSResult get(const sregex& regex) const
	{
		smatch m;
		FSResult result;
		for (const string& packageName: __get_package_names())
		{
			if (!regex_match(packageName, m, regex))
			{
				continue;
			}
			__add_package_to_result(packageName, &result);
		}
		return result;
	}
	~VersionSetGetter()
	{
		delete __cached_all_versions;
	}
};

class VersionSet
{
	const VersionSetGetter* __getter;
	bool __filtered;
	FSResult __versions;

 public:
	explicit VersionSet(const VersionSetGetter* getter)
		: __getter(getter), __filtered(false)
	{}
	VersionSet(FSResult&& versions)
		: __filtered(true), __versions(std::move(versions))
	{}
	FSResult get()
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
	FSResult get(const sregex& regex)
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
};

class CommonFS: public FS
{
 public:
	typedef vector< string > Arguments;
	virtual FSResult select(const Cache&, VersionSet&& from) const = 0;
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
	list< unique_ptr< CommonFS > > _leaves;
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
			auto parsedQuery = parseFunctionQuery(argument);
			_leaves.push_back(unique_ptr< CommonFS >(static_cast< CommonFS* >(parsedQuery.release())));
		}
	}
};

class AndFS: public AlgeFS
{
 public:
	AndFS(const Arguments& arguments)
		: AlgeFS(arguments)
	{}
	FSResult select(const Cache& cache, VersionSet&& from) const
	{
		auto result = _leaves.front()->select(cache, std::move(from));
		for (auto it = ++_leaves.begin(); it != _leaves.end(); ++it)
		{
			result = (*it)->select(cache, std::move(result));
		}
		return result;
	}
};

class OrFS: public AlgeFS
{
 public:
	OrFS(const Arguments& arguments)
		: AlgeFS(arguments)
	{}
	FSResult select(const Cache& cache, VersionSet&& from) const
	{
		auto result = _leaves.front()->select(cache, VersionSet(from));
		for (auto it = ++_leaves.begin(); it != _leaves.end(); ++it)
		{
			auto part = (*it)->select(cache, VersionSet(from));
			auto lessPredicate = [&cache](const SPCV& left, const SPCV& right)
			{
				return __spcv_less(cache, left, right);
			};
			result.merge(part, lessPredicate);
		}
		return result;
	}
};

class PredicateFS: public CommonFS
{
 protected:
	virtual bool _match(const SPCV& version) const = 0;
 public:
	FSResult select(const Cache&, VersionSet&& from) const
	{
		FSResult result = from.get();
		result.remove_if([this](const SPCV& version) { return !this->_match(version); });
		return result;
	}
};

sregex __parse_regex(const string& input)
{
	try
	{
		return sregex::compile(input, regex_constants::optimize);
	}
	catch (regex_error&)
	{
		fatal2(__("regular expression '%s' is not valid"), input);
		__builtin_unreachable();
	}
}

sregex __get_regex_from_arguments(const CommonFS::Arguments& arguments)
{
	__require_n_arguments(arguments, 1);
	return __parse_regex(arguments[0]);
}

class RegexMatcher
{
	sregex __regex;
	mutable smatch __m;

 public:
	RegexMatcher(const CommonFS::Arguments& arguments)
		: __regex(__get_regex_from_arguments(arguments))
	{}
	bool match(const string& input) const
	{
		return regex_match(input, __m, __regex);
	}
};

class PackageNameFS: public CommonFS
{
	sregex __regex;
 public:
	PackageNameFS(const Arguments& arguments)
		: __regex(__get_regex_from_arguments(arguments))
	{}
	FSResult select(const Cache&, VersionSet&& from) const
	{
		return from.get(__regex);
	}
};

class RegexMatchFS: public PredicateFS
{
	std::function< string (const SPCV&) > __get_attribute;
	RegexMatcher __regex_matcher;
 public:
	RegexMatchFS(decltype(__get_attribute) getAttribute, const Arguments& arguments)
		: __get_attribute(getAttribute), __regex_matcher(arguments)
	{}
 protected:
	bool _match(const SPCV& version) const
	{
		return __regex_matcher.match(__get_attribute(version));
	}
};

FS* constructFSByName(const string& functionName, const CommonFS::Arguments& arguments)
{
	#define CONSTRUCT_FS(name, code) if (functionName == name) { return new code; }
	#define VERSION_MEMBER(member) [](const SPCV& version) { return version-> member; }
	CONSTRUCT_FS("and", AndFS(arguments))
	CONSTRUCT_FS("or", OrFS(arguments))
	CONSTRUCT_FS("package", PackageNameFS(arguments))
	CONSTRUCT_FS("version", RegexMatchFS(VERSION_MEMBER(versionString), arguments))
	CONSTRUCT_FS("maintainer", RegexMatchFS(VERSION_MEMBER(maintainer), arguments))
	CONSTRUCT_FS("priority", RegexMatchFS([](const SPCV& version)
			{ return Version::Priorities::strings[version->priority]; }
			, arguments))
	fatal2(__("unknown selector function '%s'"), functionName);
	__builtin_unreachable();
}

vector< string > split(char delimiter, const string& input)
{
	vector< string > result;
	size_t startPosition = 0;
	size_t position;
	while (position = input.find(delimiter, startPosition), position != string::npos)
	{
		result.push_back(input.substr(startPosition, position - startPosition));
		startPosition = position+1;
	}
	result.push_back(input.substr(startPosition, input.size() - startPosition));
	return result;
}

}

unique_ptr< FS > parseFunctionQuery(const string& query)
{
	try
	{
		if (query.empty())
		{
			fatal2(__("query cannot be empty"));
		}
		auto argumentsPosition = query.find_first_of("()");
		if (query[argumentsPosition] == ')')
		{
			fatal2(__("closing bracket ')' doesn't have a corresponding opening bracket '('"));
		}
		// now we know it's surely '('
		if (query.back() != ')')
		{
			fatal2(__("the last query character is not a closing bracket ')'"));
		}
		string functionName = query.substr(0, argumentsPosition);
		auto arguments = split(',', query.substr(argumentsPosition + 1, query.size() - argumentsPosition - 2));
		return unique_ptr< FS >(constructFSByName(functionName, arguments));
	}
	catch (Exception&)
	{
		fatal2(__("unable to parse the query '%s'"), query);
		__builtin_unreachable();
	}
}

list< SPCV > selectAllVersions(const Cache& cache, const FS& functionSelector, bool binary)
{
	VersionSetGetter versionSetGetter(cache, binary);
	const CommonFS* commonFS = dynamic_cast< const CommonFS* >(&functionSelector);
	if (!commonFS)
	{
		fatal2i("selectVersion: functionSelector is not an ancestor of CommonFS");
	}
	return commonFS->select(cache, VersionSet(&versionSetGetter));
}

list< SPCV > selectBestVersions(const Cache& cache, const FS& functionSelector, bool binary)
{
	auto result = selectAllVersions(cache, functionSelector, binary);
	result.unique([](const SPCV& left, const SPCV& right) { return left->packageName == right->packageName; });
	return result;
}

vector< SPCV > list2vector(list< SPCV >&& source)
{
	vector< SPCV > result;
	std::move(source.begin(), source.end(), std::back_inserter(result));
	return result;
}
