/**************************************************************************
*   Copyright (C) 2010-2011 by Eugene V. Lyubimkin                        *
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
#include <dlfcn.h>

#include <map>

#include <cupt/config.hpp>
#include <cupt/download/uri.hpp>
#include <cupt/download/methodfactory.hpp>

#include <internal/filesystem.hpp>

namespace cupt {
namespace internal {

using std::map;
using std::multimap;

class MethodFactoryImpl
{
	typedef download::Method* (*MethodBuilder)();
	shared_ptr< const Config > __config;
	map< string, MethodBuilder > __method_builders;
	vector< void* > __dl_handles;

	void __load_methods();
	int __get_method_priority(const string& protocol, const string& methodName) const;
 public:
	MethodFactoryImpl(const shared_ptr< const Config >&);
	~MethodFactoryImpl();
	download::Method* getDownloadMethodForUri(const download::Uri& uri) const;
};


MethodFactoryImpl::MethodFactoryImpl(const shared_ptr< const Config >& config)
	: __config(config)
{
	__load_methods();
}

MethodFactoryImpl::~MethodFactoryImpl()
{
	FORIT(dlHandleIt, __dl_handles)
	{
		if (dlclose(*dlHandleIt))
		{
			warn2("unable to unload dl handle '%p': %s", *dlHandleIt, dlerror());
		}
	}
}

#ifdef CUPT_LOCAL_BUILD
	const string downloadMethodPath = "downloadmethods/";
#else
	#define QUOTED(x) QUOTED_(x)
	#define QUOTED_(x) # x
	const string downloadMethodPath = "/usr/lib/cupt2-" QUOTED(SOVERSION) "/downloadmethods/";
	#undef QUOTED
	#undef QUOTED_
#endif

void MethodFactoryImpl::__load_methods()
{
	auto debugging = __config->getBool("debug::downloader");
	auto paths = fs::glob(downloadMethodPath + "*.so");
	if (paths.empty())
	{
		warn2("no download methods found");
	}
	FORIT(pathIt, paths)
	{
		string methodName;
		{ // computing method name
			auto startPosition = pathIt->rfind('/') + 1;
			auto endPosition = pathIt->find('.', startPosition);
			methodName = pathIt->substr(startPosition, endPosition - startPosition);
			// also, it should start with 'lib'
			if (methodName.size() < 4 || methodName.compare(0, 3, "lib"))
			{
				debug2("method filename '%s' does not start with 'lib', discarding it", methodName);
			}
			methodName = methodName.substr(3);
		}

		if (__method_builders.count(methodName))
		{
			warn2("not loading another copy of download method '%s'", methodName);
			continue;
		}

		auto dlHandle = dlopen(pathIt->c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!dlHandle)
		{
			warn2("unable to load download method '%s': dlopen: %s", methodName, dlerror());
			continue;
		}
		MethodBuilder methodBuilder = reinterpret_cast< MethodBuilder >(dlsym(dlHandle, "construct"));
		if (!methodBuilder)
		{
			warn2("unable to load download method '%s': dlsym: %s", methodName, dlerror());
			if (dlclose(dlHandle))
			{
				warn2("unable to unload dl handle '%p': %s", dlHandle, dlerror());
			}
			continue;
		}
		__dl_handles.push_back(dlHandle);
		__method_builders[methodName] = methodBuilder;
		if (debugging)
		{
			debug2("loaded download method '%s'", methodName);
		}
	}
}

download::Method* MethodFactoryImpl::getDownloadMethodForUri(const download::Uri& uri) const
{
	auto protocol = uri.getProtocol();

	auto optionName = string("cupt::downloader::protocols::") + protocol + "::methods";
	auto availableHandlerNames = __config->getList(optionName);
	if (availableHandlerNames.empty())
	{
		fatal2("no download handlers defined for '%s' protocol", protocol);
	}

	// not very effective, but readable and we hardly ever get >10 handlers for same protocol
	multimap< int, string, std::greater< int > > prioritizedHandlerNames;
	FORIT(handlerNameIt, availableHandlerNames)
	{
		const string& handlerName = *handlerNameIt;
		prioritizedHandlerNames.insert(
				make_pair(__get_method_priority(protocol, handlerName), handlerName));
	}

	bool debugging = __config->getBool("debug::downloader");
	FORIT(handlerIt, prioritizedHandlerNames)
	{
		const string& handlerName = handlerIt->second;
		// some methods may be unavailable
		auto methodBuilderIt = __method_builders.find(handlerName);
		if (methodBuilderIt == __method_builders.end())
		{
			if (debugging)
			{
				debug2("download handler '%s' (priority %d) for uri '%s' is not available",
						handlerName, handlerIt->first, (string)uri);
			}
			continue;
		}
		if (debugging)
		{
			debug2("selected download handler '%s' for uri '%s'", handlerName, (string)uri);
		}

		return (methodBuilderIt->second)();
	}

	fatal2("no download handlers available");
	return NULL; // unreachable
}

int MethodFactoryImpl::__get_method_priority(const string& protocol, const string& methodName) const
{
	string optionName = string("cupt::downloader::protocols::") + protocol +
			"::methods::" + methodName + "::priority";
	auto result = __config->getInteger(optionName);
	return result ? result : 100;
}

}

namespace download {

MethodFactory::MethodFactory(const shared_ptr< const Config >& config)
{
	__impl = new internal::MethodFactoryImpl(config);
}

MethodFactory::~MethodFactory()
{
	delete __impl;
}

Method* MethodFactory::getDownloadMethodForUri(const Uri& uri) const
{
	return __impl->getDownloadMethodForUri(uri);
}

}
}

