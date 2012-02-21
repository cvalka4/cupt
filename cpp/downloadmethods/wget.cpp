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
#include <sys/time.h>
#include <sys/stat.h>

#include <cstring>
#include <thread>
#include <chrono>

#include <boost/lexical_cast.hpp>
using boost::lexical_cast;

#include <cupt/config.hpp>
#include <cupt/download/method.hpp>
#include <cupt/download/uri.hpp>
#include <cupt/pipe.hpp>

namespace cupt {

class WgetMethod: public cupt::download::Method
{
	string perform(const shared_ptr< const Config >& config, const download::Uri& uri,
			const string& targetPath, const std::function< void (const vector< string >&) >& callback)
	{
		Pipe wgetErrorStream("wget error stream");

		std::condition_variable wgetProcessFinished;

		try
		{
			ssize_t totalBytes = 0;
			{
				struct stat st;
				if (lstat(targetPath.c_str(), &st) == -1)
				{
					if (errno != ENOENT)
					{
						fatal2("stat on file '%s' failed", targetPath);
					}
				}
				else
				{
					totalBytes = st.st_size;
					callback(vector< string > { "downloading",
							lexical_cast< string >(totalBytes), lexical_cast< string >(0)});
				}
			}

			// wget executor
			vector< string > p; // array to put parameters
			{
				p.push_back("env");
				auto proxy = getAcquireSuboptionForUri(config, uri, "proxy");
				if (!proxy.empty() && proxy != "DIRECT")
				{
					p.push_back(uri.getProtocol() + "_proxy=" + proxy);
				}
				p.push_back("wget"); // passed as a binary name, not parameter
				p.push_back("--continue");
				p.push_back(string("--tries=") + lexical_cast< string >(config->getInteger("acquire::retries")+1));
				auto maxSpeedLimit = getIntegerAcquireSuboptionForUri(config, uri, "dl-limit");
				if (maxSpeedLimit)
				{
					p.push_back(string("--limit-rate=") + lexical_cast< string >(maxSpeedLimit) + "k");
				}
				if (proxy == "DIRECT")
				{
					p.push_back("--no-proxy");
				}
				if (uri.getProtocol() != "http" || !config->getBool("acquire::http::allowredirect"))
				{
					p.push_back("--max-redirect=0");
				}
				auto timeout = getIntegerAcquireSuboptionForUri(config, uri, "timeout");
				if (timeout)
				{
					p.push_back(string("--timeout=") + lexical_cast< string >(timeout));
				}
				p.push_back(string(uri));
				p.push_back(string("--output-document=") + targetPath);
			}

			if (dup2(wgetErrorStream.getWriterFd(), STDOUT_FILENO) == -1) // redirecting stdout
			{
				fatal2("unable to redirect wget standard output: dup2 failed");
			}
			if (dup2(wgetErrorStream.getWriterFd(), STDERR_FILENO) == -1) // redirecting stderr
			{
				fatal2("unable to redirect wget error stream: dup2 failed");
			}

			string errorString;
			std::thread readWgetErrorsThread([&errorString](int fd)
			{
				FILE* inputHandle = fdopen(fd, "r");
				if (!inputHandle)
				{
					fatal2("unable to fdopen wget error stream");
				}
				char buf[1024];
				while (fgets(buf, sizeof(buf), inputHandle), !feof(inputHandle))
				{
					errorString += buf;
				}
			}, wgetErrorStream.getReaderFd());

			std::thread downloadingStatsThread([&targetPath, &totalBytes, &wgetProcessFinished, &callback]()
			{
				std::mutex conditionMutex;
				std::unique_lock< std::mutex > conditionMutexLock(conditionMutex);
				while (wgetProcessFinished.wait_for(conditionMutexLock, std::chrono::milliseconds(100)) != std::cv_status::timeout)
				{
					struct stat st;
					if (lstat(targetPath.c_str(), &st) == -1)
					{
						if (errno != ENOENT) // wget haven't created the file yet
						{
							fatal2("stat on file '%s' failed", targetPath);
						}
					}
					else
					{
						auto newTotalBytes = st.st_size;
						if (newTotalBytes != totalBytes)
						{
							callback(vector< string >{ "downloading",
									lexical_cast< string >(newTotalBytes),
									lexical_cast< string >(newTotalBytes - totalBytes) });
							totalBytes = newTotalBytes;
						}
					}
				}
			});

			auto result = ::system(join(" ", p).c_str());
			wgetProcessFinished.notify_all();
			wgetErrorStream.useAsReader(); // close the writing part
			readWgetErrorsThread.join();
			downloadingStatsThread.join();

			if (result == -1)
			{
				fatal2e("failed to launch a wget process");
			}
			else if (!result)
			{
				if (WIFEXITED(result))
				{
					return errorString;
				}
				else
				{
					fatal2("wget process returned an error: %s", getWaitStatusDescription(result));
				}
			}
			return ""; // unreachable
		}
		catch (Exception& e)
		{
			return format2("download method error: %s", e.what());
		}
	}
};

}

extern "C"
{
	cupt::download::Method* construct()
	{
		return new cupt::WgetMethod();
	}
}
