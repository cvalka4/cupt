/**************************************************************************
*   Copyright (C) 2011 by Eugene V. Lyubimkin                             *
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
#include <cupt/system/resolver.hpp>

namespace cupt {
namespace system {

Resolver::RelationExpressionReason::RelationExpressionReason(
		const shared_ptr< const BinaryVersion >& version_,
		BinaryVersion::RelationTypes::Type dependencyType_,
		const cache::RelationExpression& relationExpression_)
	: version(version_), dependencyType(dependencyType_),
	relationExpression(relationExpression_)
{}

Resolver::SynchronizationReason::SynchronizationReason(const string& packageName_)
	: packageName(packageName_)
{}

}
}