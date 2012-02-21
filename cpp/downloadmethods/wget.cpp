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

		string errorString;
		std::thread readWgetErrors([&errorString]()
		{
			FILE* inputHandle = fdopen(wgetErrorStream.getReaderFd(), "r");
			if (!inputHandle)
			{
				fatal2("unable to fdopen wget error stream");
			}
			char buf[1024];
			while (fgets(buf, sizeof(buf), inputHandle), !feof(inputHandle))
			{
				errorString += buf;
			}

			int workerStatus;
			if (WEXITSTATUS(workerStatus) != 0)
			{
				return errorString;
			}
			else
			{
				return "";
			}
		});

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

			auto wgetPid = fork();
			if (wgetPid == -1)
			{
				fatal2("unable to fork");
			}
			if (wgetPid)
			{
				childPid = wgetPid;
				// still wrapper
				int waitResult;
				int childStatus;
				struct timespec ts;
				ts.tv_sec = 0;
				ts.tv_nsec = 100 * 1000 * 1000; // 100 milliseconds
				while (waitResult = waitpid(wgetPid, &childStatus, WNOHANG), !waitResult)
				{
					if (nanosleep(&ts, NULL) == -1)
					{
						if (errno != EINTR)
						{
							fatal2("nanosleep failed");
						}
					}
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
				if (waitResult == -1)
				{
					fatal2("waitpid failed");
				}
				if (!WIFEXITED(childStatus) || WEXITSTATUS(childStatus) != 0)
				{
					_exit(EXIT_FAILURE);
				}
			}
			else
			{
				// wget executor
				vector< string > p; // temporary array to put parameters
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

				vector< char* > params;
				FORIT(it, p)
				{
					params.push_back(strdup(it->c_str()));
				}
				params.push_back(NULL);

				if (dup2(wgetErrorStream.getWriterFd(), STDOUT_FILENO) == -1) // redirecting stdout
				{
					fatal2("unable to redirect wget standard output: dup2 failed");
				}
				if (dup2(wgetErrorStream.getWriterFd(), STDERR_FILENO) == -1) // redirecting stderr
				{
					fatal2("unable to redirect wget error stream: dup2 failed");
				}
				execv("/usr/bin/env", &params[0]);
				// if we are here, exec returned an error
				fatal2("unable to launch wget process");
			}
		}
		catch (Exception& e)
		{
			char nonWgetError[] = "download method error: ";
			write(wgetErrorStream.getWriterFd(), nonWgetError, sizeof(nonWgetError) - 1);
			write(wgetErrorStream.getWriterFd(), e.what(), strlen(e.what()));
			write(wgetErrorStream.getWriterFd(), "\n", 1);
			exit(EXIT_FAILURE);
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
