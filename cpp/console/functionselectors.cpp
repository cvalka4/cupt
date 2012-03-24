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

#include <cupt/cache/releaseinfo.hpp>
#include <cupt/cache/binarypackage.hpp>
#include <cupt/cache/sourcepackage.hpp>
#include <cupt/cache/binaryversion.hpp>

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
struct SpcvLess
{
 private:
	const Cache& __cache;
 public:
	SpcvLess(const Cache& cache) : __cache(cache) {}
	bool operator()(const SPCV& left, const SPCV& right)
	{
		return __spcv_less(__cache, left, right);
	}
};

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
	std::map< string, FSResult > __variables;

	VersionSet(const VersionSet& from, FSResult&& versions)
		: __filtered(true), __versions(std::move(versions)), __variables(from.__variables)
	{}
 public:
	explicit VersionSet(const VersionSetGetter* getter)
		: __getter(getter), __filtered(false)
	{}
	VersionSet generate(FSResult&& versions) const
	{
		return VersionSet(*this, std::move(versions));
	}
	const FSResult& get() const
	{
		if (__filtered)
		{
			return __versions;
		}
		else
		{
			return __getter->getAll();
		}
	}
	FSResult get(const sregex& regex) const
	{
		if (__filtered)
		{
			smatch m;
			FSResult result(__versions);
			result.remove_if([&regex, &m](const SPCV& version)
			{
				return !regex_match(version->packageName, m, regex);
			});
			return result;
		}
		else
		{
			return __getter->get(regex);
		}
	}
	void setVariable(const string& name, FSResult&& versions)
	{
		__variables[name] = std::move(versions);
	}
	const FSResult& getFromVariable(const string& name) const
	{
		auto it = __variables.find(name);
		if (it == __variables.end())
		{
			fatal2("the variable '%s' is not defined", name);
		}
		return it->second;
	}
	VersionSet getUnfiltered() const
	{
		VersionSet result(__getter);
		result.__variables = this->__variables;
		return result;
	}
	bool isFiltered() const
	{
		return __filtered;
	}
};

class CommonFS: public FS
{
 public:
	typedef vector< string > Arguments;
	virtual FSResult select(const Cache&, const VersionSet& from) const = 0;
};

unique_ptr< CommonFS > internalParseFunctionQuery(const string& query, bool binary);

void __require_n_arguments(const CommonFS::Arguments& arguments, size_t n)
{
	if (arguments.size() != n)
	{
		fatal2(__("the function requires exactly %zu arguments"), n);
	}
}

FSResult __filter_through(const Cache& cache, const FSResult& versions, const VersionSet& allowedSet)
{
	if (allowedSet.isFiltered())
	{
		const auto& allowedVersions = allowedSet.get();
		FSResult result;
		std::set_intersection(allowedVersions.begin(), allowedVersions.end(),
				versions.begin(), versions.end(),
				std::back_inserter(result), SpcvLess(cache));
		return result;
	}
	else
	{
		return versions;
	}
}

class DefineVariableFS: public CommonFS
{
	string __name;
	unique_ptr< CommonFS > __value_fs;
	unique_ptr< CommonFS > __leaf_fs;
 public:
	DefineVariableFS(bool binary, const Arguments& arguments)
	{
		__require_n_arguments(arguments, 3);
		__name = arguments[0];
		__value_fs = internalParseFunctionQuery(arguments[1], binary);
		__leaf_fs = internalParseFunctionQuery(arguments[2], binary);
	}
	FSResult select(const Cache& cache, const VersionSet& from) const
	{
		VersionSet modifiedFrom(from);
		modifiedFrom.setVariable(__name,
				__value_fs->select(cache, from.getUnfiltered()));
		return __leaf_fs->select(cache, modifiedFrom);
	}
};

class ExtractVariableFS: public CommonFS
{
	const string __name;
 public:
	ExtractVariableFS(const string& name, const Arguments& arguments)
		: __name(name)
	{
		__require_n_arguments(arguments, 0);
	}
	FSResult select(const Cache& cache, const VersionSet& from) const
	{
		return __filter_through(cache, from.getFromVariable(__name), from);
	}
};

class AlgeFS: public CommonFS
{
 protected:
	list< unique_ptr< CommonFS > > _leaves;
 public:
	// postcondition: _leaves are not empty
	AlgeFS(bool binary, const Arguments& arguments)
	{
		if (arguments.empty())
		{
			fatal2(__("the function should have at least one argument"));
		}
		for (const auto& argument: arguments)
		{
			_leaves.push_back(internalParseFunctionQuery(argument, binary));
		}
	}
};

class AndFS: public AlgeFS
{
 public:
	AndFS(bool binary, const Arguments& arguments)
		: AlgeFS(binary, arguments)
	{}
	FSResult select(const Cache& cache, const VersionSet& from) const
	{
		auto result = _leaves.front()->select(cache, from);
		for (auto it = ++_leaves.begin(); it != _leaves.end(); ++it)
		{
			result = (*it)->select(cache, from.generate(std::move(result)));
		}
		return result;
	}
};

class NotFS: public AlgeFS
{
	static const Arguments& __check_and_return_arguments(const Arguments& arguments)
	{
		__require_n_arguments(arguments, 1);
		return arguments;
	}
 public:
	NotFS(bool binary, const Arguments& arguments)
		: AlgeFS(binary, __check_and_return_arguments(arguments))
	{}
	FSResult select(const Cache& cache, const VersionSet& from) const
	{
		const auto& fromVersions = from.get();
		auto notVersions = _leaves.front()->select(cache, from);
		FSResult result;
		std::set_difference(fromVersions.begin(), fromVersions.end(),
				notVersions.begin(), notVersions.end(),
				std::back_inserter(result), SpcvLess(cache));
		return result;
	}
};

class XorFS: public AlgeFS
{
	static const Arguments& __check_and_return_arguments(const Arguments& arguments)
	{
		__require_n_arguments(arguments, 2);
		return arguments;
	}
 public:
	XorFS(bool binary, const Arguments& arguments)
		: AlgeFS(binary, __check_and_return_arguments(arguments))
	{}
	FSResult select(const Cache& cache, const VersionSet& from) const
	{
		auto leftVersions = _leaves.front()->select(cache, from);
		auto rightVersions = _leaves.back()->select(cache, from);
		FSResult result;
		std::set_symmetric_difference(leftVersions.begin(), leftVersions.end(),
				rightVersions.begin(), rightVersions.end(),
				std::back_inserter(result), SpcvLess(cache));
		return result;
	}
};

// for determining, is function selector binary or source
class BinaryTagDummyFS: public CommonFS
{
	unique_ptr< CommonFS > __real_fs;
 public:
	BinaryTagDummyFS(unique_ptr< CommonFS >&& realFS)
		: __real_fs(std::move(realFS))
	{}
	FSResult select(const Cache& cache, const VersionSet& from) const
	{
		return __real_fs->select(cache, from);
	}
};

class OrFS: public AlgeFS
{
 public:
	OrFS(bool binary, const Arguments& arguments)
		: AlgeFS(binary, arguments)
	{}
	FSResult select(const Cache& cache, const VersionSet& from) const
	{
		auto result = _leaves.front()->select(cache, from);
		SpcvLess lessPredicate(cache);
		for (auto it = ++_leaves.begin(); it != _leaves.end(); ++it)
		{
			auto part = (*it)->select(cache, from);
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
	FSResult select(const Cache&, const VersionSet& from) const
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
	FSResult select(const Cache&, const VersionSet& from) const
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

class SourceRegexMatchFS: public PredicateFS
{
	std::function< string (const Version::Source&) > __get_source_attribute;
	RegexMatcher __regex_matcher;
 public:
	SourceRegexMatchFS(decltype(__get_source_attribute) getter, const Arguments& arguments)
		: __get_source_attribute(getter), __regex_matcher(arguments)
	{}
 protected:
	bool _match(const SPCV& version) const
	{
		for (const auto& source: version->sources)
		{
			if (__regex_matcher.match(__get_source_attribute(source)))
			{
				return true;
			}
		}
		return false;
	}
};

class BoolMatchFS: public PredicateFS
{
	std::function< bool (const SPCV&) > __get_attribute;
 public:
	BoolMatchFS(decltype(__get_attribute) getAttribute, const Arguments& arguments)
		: __get_attribute(getAttribute)
	{
		__require_n_arguments(arguments, 0);
	}
	bool _match(const SPCV& version) const
	{
		return __get_attribute(version);
	}
};

class OtherFieldRegexMatchFS: public RegexMatchFS
{
	string __field_name;
	static string __extract_second_argument(const Arguments& arguments)
	{
		__require_n_arguments(arguments, 2);
		return arguments[1];
	}
 public:
	OtherFieldRegexMatchFS(const Arguments& arguments)
		: RegexMatchFS([this](const SPCV& version) -> string
				{
					if (!version->others)
					{
						return string();
					}
					auto it = version->others->find(this->__field_name);
					if (it != version->others->end())
					{
						return it->second;
					}
					else
					{
						return string();
					}
				}, { __extract_second_argument(arguments) }),
		__field_name(arguments[0])
	{}
};

class TransformFS: public CommonFS
{
	unique_ptr< CommonFS > __leaf;
 protected:
	virtual FSResult _transform(const Cache& cache, const SPCV& version) const = 0;
 public:
	TransformFS(bool binary, const Arguments& arguments)
	{
		__require_n_arguments(arguments, 1);
		__leaf = internalParseFunctionQuery(arguments[0], binary);
	}
	FSResult select(const Cache& cache, const VersionSet& from) const
	{
		SpcvLess spcvLess(cache);
		FSResult allTransformed;
		for (const auto& version: __leaf->select(cache, from.getUnfiltered()))
		{
			auto transformedList = _transform(cache, version);
			allTransformed.merge(transformedList, spcvLess);
		}
		return __filter_through(cache, allTransformed, from);
	}
};

class DependencyFS: public TransformFS
{
	const BinaryVersion::RelationTypes::Type __relation_type;
 public:
	DependencyFS(BinaryVersion::RelationTypes::Type relationType, const Arguments& arguments)
		: TransformFS(true, arguments), __relation_type(relationType)
	{}
 protected:
	FSResult _transform(const Cache& cache, const SPCV& version) const
	{
		SpcvLess spcvLess(cache);
		FSResult result;

		auto binaryVersion = static_pointer_cast< const BinaryVersion >(version);
		for (const auto& relationExpression: binaryVersion->relations[__relation_type])
		{
			auto satisfyingVersions = cache.getSatisfyingVersions(relationExpression);
			std::sort(satisfyingVersions.begin(), satisfyingVersions.end(), spcvLess);
			list< SPCV > sortedList;
			std::move(satisfyingVersions.begin(), satisfyingVersions.end(),
					std::back_inserter(sortedList));
			result.merge(sortedList, spcvLess);
		}
		return result;
	}
};

///

CommonFS* constructFSByName(const string& functionName, const CommonFS::Arguments& arguments, bool binary)
{
	#define VERSION_MEMBER(member) [](const SPCV& version) { return version-> member; }
	#define VERSION_RELEASE_MEMBER(member) [](const Version::Source& source) { return source.release-> member; }
	#define BINARY_VERSION_MEMBER(member) [](const SPCV& version) \
			{ return static_cast< const BinaryVersion* >(version.get())-> member; }
	#define CONSTRUCT_FS(name, code) if (functionName == name) { return new code; }
	#define CONSTRUCT_RELEASE_MEMBER_FS(name, member) \
			CONSTRUCT_FS(name, SourceRegexMatchFS(VERSION_RELEASE_MEMBER(member), arguments))

	// variables
	if (functionName.front() == '_')
	{
		return new ExtractVariableFS(functionName, arguments);
	}
	CONSTRUCT_FS("with", DefineVariableFS(binary, arguments))
	// logic
	CONSTRUCT_FS("and", AndFS(binary, arguments))
	CONSTRUCT_FS("or", OrFS(binary, arguments))
	CONSTRUCT_FS("not", NotFS(binary, arguments))
	CONSTRUCT_FS("xor", XorFS(binary, arguments))
	// common
	CONSTRUCT_FS("package", PackageNameFS(arguments))
	CONSTRUCT_FS("version", RegexMatchFS(VERSION_MEMBER(versionString), arguments))
	CONSTRUCT_FS("maintainer", RegexMatchFS(VERSION_MEMBER(maintainer), arguments))
	CONSTRUCT_FS("priority", RegexMatchFS([](const SPCV& version)
			{ return Version::Priorities::strings[version->priority]; }
			, arguments))
	CONSTRUCT_FS("section", RegexMatchFS(VERSION_MEMBER(section), arguments))
	CONSTRUCT_FS("signed", BoolMatchFS(VERSION_MEMBER(isVerified()), arguments))
	CONSTRUCT_FS("field", OtherFieldRegexMatchFS(arguments))
	CONSTRUCT_RELEASE_MEMBER_FS("archive", archive)
	CONSTRUCT_RELEASE_MEMBER_FS("codename", codename)
	CONSTRUCT_RELEASE_MEMBER_FS("component", component)
	CONSTRUCT_RELEASE_MEMBER_FS("release-version", version)
	CONSTRUCT_RELEASE_MEMBER_FS("vendor", vendor)
	CONSTRUCT_RELEASE_MEMBER_FS("release-origin", baseUri)
	if (binary)
	{
		CONSTRUCT_FS("source-package", RegexMatchFS(BINARY_VERSION_MEMBER(sourcePackageName), arguments))
		CONSTRUCT_FS("source-version", RegexMatchFS(BINARY_VERSION_MEMBER(sourceVersionString), arguments))
		CONSTRUCT_FS("essential", BoolMatchFS(BINARY_VERSION_MEMBER(essential), arguments))
		CONSTRUCT_FS("installed", BoolMatchFS(BINARY_VERSION_MEMBER(isInstalled()), arguments))
		CONSTRUCT_FS("depends", DependencyFS(BinaryVersion::RelationTypes::Depends, arguments))
	}
	fatal2(__("unknown %s selector function '%s'"), binary ? __("binary") : __("source"), functionName);
	__builtin_unreachable();
}

vector< string > split(const string& input)
{
	if (input.empty())
	{
		return {};
	}
	vector< string > result;
	size_t argumentStartPosition = 0;
	size_t position = 0;
	size_t level = 0;
	while (position = input.find_first_of(",()/", position), position != string::npos)
	{
		switch (input[position])
		{
			case ',':
				if (level == 0)
				{
					result.push_back(input.substr(argumentStartPosition, position - argumentStartPosition));
					argumentStartPosition = position+1;
				}
				break;
			case '(':
				++level;
				break;
			case ')':
				if (level == 0)
				{
					fatal2(__("unexpected closing bracket ')' after '%s'"), input.substr(0, position));
				}
				--level;
				break;
			case '/': // quoting
				position = input.find('/', position+1);
				if (position == string::npos)
				{
					fatal2(__("unable to find closing quoting character '/'"));
				}
		}
		++position;
	}
	if (level != 0)
	{
		fatal2(__("too few closing brackets"));
	}
	result.push_back(input.substr(argumentStartPosition, input.size() - argumentStartPosition));
	return result;
}

void stripArgumentQuotes(string& argument)
{
	if (argument.size() >= 2)
	{
		if (argument.front() == '/' && argument.back() == '/')
		{
			argument = argument.substr(1, argument.size()-2);
		}
	}
}

unique_ptr< CommonFS > internalParseFunctionQuery(const string& query, bool binary)
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
		auto arguments = split(query.substr(argumentsPosition + 1, query.size() - argumentsPosition - 2));
		for (string& argument: arguments)
		{
			stripArgumentQuotes(argument);
		}
		return unique_ptr< CommonFS >(constructFSByName(functionName, arguments, binary));
	}
	catch (Exception&)
	{
		fatal2(__("unable to parse the query '%s'"), query);
		__builtin_unreachable();
	}
}

}

unique_ptr< FS > parseFunctionQuery(const string& query, bool binary)
{
	Cache::memoize = true;
	Package::memoize = true;
	auto result = internalParseFunctionQuery(query, binary);
	if (binary)
	{
		unique_ptr< CommonFS > newResult(new BinaryTagDummyFS(std::move(result)));
		result.swap(newResult);
	}
	return std::move(result);
}

list< SPCV > selectAllVersions(const Cache& cache, const FS& functionSelector)
{
	const CommonFS* commonFS = dynamic_cast< const CommonFS* >(&functionSelector);
	if (!commonFS)
	{
		fatal2i("selectVersion: functionSelector is not an ancestor of CommonFS");
	}
	bool binary = (dynamic_cast< const BinaryTagDummyFS* >(commonFS));
	VersionSetGetter versionSetGetter(cache, binary);
	return commonFS->select(cache, VersionSet(&versionSetGetter));
}

list< SPCV > selectBestVersions(const Cache& cache, const FS& functionSelector)
{
	auto result = selectAllVersions(cache, functionSelector);
	result.unique([](const SPCV& left, const SPCV& right) { return left->packageName == right->packageName; });
	return result;
}

