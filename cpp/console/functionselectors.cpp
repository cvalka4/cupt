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
typedef FunctionSelector FS;

namespace {

class CommonFS: public FS
{
 public:
	typedef list< string > Arguments;
	virtual FS::Result select(const Cache& cache, FS::Result&& from);
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
