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

#include <cupt/cache.hpp>
#include <cupt/cache/binarypackage.hpp>

#include <internal/nativeresolver/solution.hpp>
#include <internal/nativeresolver/dependencygraph.hpp>

namespace cupt {
namespace internal {

using std::make_pair;

PackageEntry::PackageEntry()
	: sticked(false), autoremoved(false)
{}

bool PackageEntry::isModificationAllowed(const dg::Element* elementPtr) const
{
	auto findResult = std::find(rejectedConflictors.begin(),
			rejectedConflictors.end(), elementPtr);
	return (findResult == rejectedConflictors.end());
}

template < class data_t, class Comparator, class KeyGetter >
class PackageEntryMapBase
{
 public:
	typedef const dg::Element* key_t;
	typedef data_t value_type; // for set_union
	typedef vector< data_t > container_t;
	typedef data_t* iterator_t;
	typedef const data_t* const_iterator_t;
 private:
	container_t __container;
	typename container_t::iterator __position_to_iterator(const_iterator_t position)
	{
		return static_cast< typename container_t::iterator >(const_cast< iterator_t >(position));
	}
 public:
	size_t size() const { return __container.size(); }
	void reserve(size_t size) { __container.reserve(size); }
	const_iterator_t begin() const { return &*__container.begin(); }
	const_iterator_t end() const { return &*__container.end(); }
	const_iterator_t lower_bound(const key_t& key) const
	{
		return std::lower_bound(begin(), end(), key, Comparator());
	}
	iterator_t lower_bound(const key_t& key)
	{
		return const_cast< iterator_t >(((const PackageEntryMapBase*)this)->lower_bound(key));
	}
	const_iterator_t find(const key_t& key) const
	{
		auto result = lower_bound(key);
		if (result != end() && KeyGetter()(*result) != key)
		{
			result = end();
		}
		return result;
	}
	// this insert() is called only for unexisting elements
	iterator_t insert(const_iterator_t position, data_t&& data)
	{
		auto distance = position - begin();
		__container.insert(__position_to_iterator(position), std::move(data));
		return const_cast< iterator_t >(begin()) + distance;
	}
	void erase(const_iterator_t position)
	{
		__container.erase(__position_to_iterator(position));
	}
	void push_back(const data_t& data)
	{
		__container.push_back(data);
	}
};

struct PackageEntryMapComparator
{
	bool operator()(const pair< const dg::Element*, PackageEntry >& data, const dg::Element* key) const
	{ return data.first < key; }
};
struct PackageEntryMapKeyGetter
{
	const dg::Element* operator()(const pair< const dg::Element*, PackageEntry >& data)
	{ return data.first; }
};
class PackageEntryMap: public PackageEntryMapBase<
		pair< const dg::Element*, PackageEntry >,
		PackageEntryMapComparator, PackageEntryMapKeyGetter >
{
 public:
	mutable size_t forkedCount;

	PackageEntryMap()
		: forkedCount(0)
	{}
};


struct PackageEntrySetComparator
{
	bool operator()(const dg::Element* data, const dg::Element* key) const
	{ return data < key; }
};
struct PackageEntrySetKeyGetter
{
	const dg::Element* operator()(const dg::Element* data) { return data; }
};
class PackageEntrySet: public PackageEntryMapBase< const dg::Element*,
		PackageEntrySetComparator, PackageEntrySetKeyGetter >
{};


SolutionStorage::SolutionStorage(const Config& config, const Cache& cache)
	: __next_free_id(1), __dependency_graph(config, cache)
{}

shared_ptr< Solution > SolutionStorage::cloneSolution(const shared_ptr< Solution >& source)
{
	shared_ptr< Solution > cloned(new Solution);
	cloned->score = source->score;
	cloned->level = source->level;
	cloned->id = __next_free_id++;
	cloned->finished = false;

	cloned->__parent = source;

	// other part should be done by calling prepare outside

	return cloned;
}

const GraphCessorListType& SolutionStorage::getSuccessorElements(const dg::Element* elementPtr) const
{
	return __dependency_graph.getSuccessorsFromPointer(elementPtr);
}

const GraphCessorListType& SolutionStorage::getPredecessorElements(const dg::Element* elementPtr) const
{
	return __dependency_graph.getPredecessorsFromPointer(elementPtr);
}

const forward_list< const dg::Element* >& SolutionStorage::getConflictingElements(
		const dg::Element* elementPtr)
{
	static const forward_list< const dg::Element* > nullList;
	auto relatedElementPtrsPtr = elementPtr->getRelatedElements();
	return relatedElementPtrsPtr? *relatedElementPtrsPtr : nullList;
}

bool SolutionStorage::simulateSetPackageEntry(const Solution& solution,
		const dg::Element* elementPtr, const dg::Element** conflictingElementPtrPtr) const
{
	const forward_list< const dg::Element* >& conflictingElementPtrs =
			getConflictingElements(elementPtr);
	FORIT(conflictingElementPtrIt, conflictingElementPtrs)
	{
		if (*conflictingElementPtrIt == elementPtr)
		{
			continue;
		}
		if (auto packageEntryPtr = solution.getPackageEntry(*conflictingElementPtrIt))
		{
			// there may be only one conflicting element in the solution
			*conflictingElementPtrPtr = *conflictingElementPtrIt;

			return (!packageEntryPtr->sticked && packageEntryPtr->isModificationAllowed(elementPtr));
		}
	}

	// no conflicting elements in this solution
	*conflictingElementPtrPtr = NULL;
	if (auto versionElement = dynamic_cast< const dg::VersionElement* >(elementPtr))
	{
		if (versionElement->version)
		{
			*conflictingElementPtrPtr = const_cast< dg::DependencyGraph& >
					(__dependency_graph).getCorrespondingEmptyElement(elementPtr);
		}
	}
	return true;
}

void SolutionStorage::setRejection(Solution& solution, const dg::Element* elementPtr)
{
	const dg::Element* conflictingElementPtr;
	simulateSetPackageEntry(solution, elementPtr, &conflictingElementPtr);
	if (!conflictingElementPtr)
	{
		return;
	}
	auto conflictorPackageEntryPtr = solution.getPackageEntry(conflictingElementPtr);

	PackageEntry packageEntry = (conflictorPackageEntryPtr ?
			PackageEntry(*conflictorPackageEntryPtr) : PackageEntry());

	packageEntry.rejectedConflictors.push_front(elementPtr);
	setPackageEntry(solution, conflictingElementPtr,
			std::move(packageEntry), NULL);
}

void SolutionStorage::setPackageEntry(Solution& solution,
		const dg::Element* elementPtr, PackageEntry&& packageEntry,
		const dg::Element* conflictingElementPtr)
{
	__dependency_graph.unfoldElement(elementPtr);

	auto it = solution.__added_entries->lower_bound(elementPtr);
	if (it == solution.__added_entries->end() || it->first != elementPtr)
	{
		// there is no modifiable element in this solution
		solution.__added_entries->insert(it, make_pair(elementPtr, std::move(packageEntry)));

		if (conflictingElementPtr)
		{
			auto forRemovalIt = solution.__added_entries->find(conflictingElementPtr);
			if (forRemovalIt != solution.__added_entries->end())
			{
				solution.__added_entries->erase(forRemovalIt);
			}
			// may be present in master too, act safe
			solution.__removed_entries->insert(
					solution.__removed_entries->lower_bound(conflictingElementPtr),
					(const dg::Element*)conflictingElementPtr);
		}
	}
	else
	{
		if (conflictingElementPtr)
		{
			fatal2("internal error: conflicting elements in __added_entries: solution '%u', in '%s', out '%s'",
					solution.id, elementPtr->toString(), conflictingElementPtr->toString());
		}
		it->second = std::move(packageEntry);
	}
}

void SolutionStorage::prepareForResolving(Solution& initialSolution,
			const map< string, shared_ptr< const BinaryVersion > >& oldPackages,
			const map< string, dg::InitialPackageEntry >& initialPackages)
{
	auto source = __dependency_graph.fill(oldPackages, initialPackages);

	auto comparator = [](const pair< const dg::Element*, PackageEntry >& left,
			const pair< const dg::Element*, PackageEntry >& right)
	{
		return left.first < right.first;
	};
	std::sort(source.begin(), source.end(), comparator);

	initialSolution.__added_entries->reserve(source.size());
	FORIT(it, source)
	{
		__dependency_graph.unfoldElement(it->first);
		initialSolution.__added_entries->push_back(*it);
	}
}

bool SolutionStorage::verifyElement(const Solution& solution,
		const dg::Element* elementPtr) const
{
	const GraphCessorListType& successorElementPtrs =
			getSuccessorElements(elementPtr);
	FORIT(elementPtrIt, successorElementPtrs)
	{
		if (solution.getPackageEntry(*elementPtrIt))
		{
			return true;
		}
	}

	// second try, check for non-present empty elements as they are virtually present
	FORIT(elementPtrIt, successorElementPtrs)
	{
		if (auto versionElement = dynamic_cast< const dg::VersionElement* >(*elementPtrIt))
		{
			if (!versionElement->version)
			{
				const dg::Element* conflictorPtr;
				if (simulateSetPackageEntry(solution, versionElement, &conflictorPtr), !conflictorPtr)
				{
					return true;
				}
			}
		}
	}

	return false;
}

const dg::Element* SolutionStorage::getCorrespondingEmptyElement(const dg::Element* elementPtr)
{
	return __dependency_graph.getCorrespondingEmptyElement(elementPtr);
}

void SolutionStorage::unfoldElement(const dg::Element* elementPtr)
{
	__dependency_graph.unfoldElement(elementPtr);
}


Solution::Solution()
	: id(0), level(0), finished(false), score(0)
{
	__added_entries.reset(new PackageEntryMap);
	__removed_entries.reset(new PackageEntrySet);
}

void Solution::prepare()
{
	if (!__parent)
	{
		fatal2("internal error: undefined master solution");
	}

	if (!__parent->__master_entries)
	{
		// parent solution is a master solution, build a slave on top of it
		__master_entries = __parent->__added_entries;
		*__removed_entries = (*__parent->__removed_entries);
	}
	else
	{
		// this a slave solution
		size_t& forkedCount = __parent->__master_entries->forkedCount;
		forkedCount += __parent->__added_entries->size();
		if (forkedCount > __parent->__master_entries->size())
		{
			forkedCount = 0;

			// master solution is overdiverted, build new master one
			__added_entries->reserve(__parent->__added_entries->size() +
					__parent->__master_entries->size());

			class RepackInsertIterator: public std::iterator< std::output_iterator_tag, PackageEntryMap::value_type >
			{
				PackageEntryMap& __target;
				PackageEntrySet& __exceptions;
			 public:
				RepackInsertIterator(PackageEntryMap& target, PackageEntrySet& exceptions)
					: __target(target), __exceptions(exceptions) {}
				RepackInsertIterator& operator++() { return *this; }
				RepackInsertIterator& operator*() { return *this; }
				void operator=(const PackageEntryMap::value_type& data)
				{
					auto presentIt = __exceptions.find(data.first);
					if (presentIt == __exceptions.end())
					{
						__target.push_back(data);
					}
				}
			};
			struct Comparator
			{
				bool operator()(const PackageEntryMap::value_type& left, const PackageEntryMap::value_type& right)
				{ return left.first < right.first; }
			};
			// it's important that parent's __added_entries come first,
			// if two elements are present in both (i.e. an element is overriden)
			// the new version of an element will be written
			std::set_union(__parent->__added_entries->begin(), __parent->__added_entries->end(),
					__parent->__master_entries->begin(), __parent->__master_entries->end(),
					RepackInsertIterator(*__added_entries, *__parent->__removed_entries), Comparator());
		}
		else
		{
			// build new slave solution from current
			__master_entries = __parent->__master_entries;
			*__added_entries = *(__parent->__added_entries);
			*__removed_entries = *(__parent->__removed_entries);
		}
	}

	insertedElementPtrs = __parent->insertedElementPtrs;
	__parent.reset();
}

vector< const dg::Element* > Solution::getElements() const
{
	vector< const dg::Element* > result;

	if (__master_entries)
	{
		FORIT(it, *__master_entries)
		{
			if (__removed_entries->find(it->first) == __removed_entries->end())
			{
				result.push_back(it->first);
			}
		}
	}
	auto middleSize = result.size();
	FORIT(it, *__added_entries)
	{
		result.push_back(it->first);
	}

	std::inplace_merge(result.begin(), result.begin() + middleSize, result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());

	return result;
}

vector< pair< const dg::Element*, PackageEntry::BrokenSuccessor > > Solution::getBrokenPairs() const
{
	vector< pair< const dg::Element*, PackageEntry::BrokenSuccessor > > result;
	auto isEligible = [](PackageEntryMap::const_iterator_t it) -> bool
	{
		return !it->second.brokenSuccessors.empty();
	};
	auto processEntry = [this, &result, &isEligible](PackageEntryMap::const_iterator_t it)
	{
		if (isEligible(it))
		{
			if (__removed_entries->find(it->first) == __removed_entries->end())
			{
				FORIT(brokenSuccessorIt, it->second.brokenSuccessors)
				{
					result.push_back(make_pair(it->first, *brokenSuccessorIt));
				}
			}
		}
	};

	auto masterIt = __master_entries ? __master_entries->begin() : __added_entries->end();
	auto masterEnd = __master_entries ? __master_entries->end() : __added_entries->end();
	auto ownIt = __added_entries->begin();
	auto ownEnd = __added_entries->end();

	// it's, surprisingly, several times faster than std::set_union due to no indirection
	while (masterIt != masterEnd && ownIt != ownEnd)
	{
		// speeding up a bit, usually most of masterIt won't be included anyway
		if (!isEligible(masterIt))
		{
			++masterIt;
			continue;
		}

		if (masterIt->first < ownIt->first)
		{
			processEntry(masterIt);
			++masterIt;
		}
		else if (masterIt->first > ownIt->first)
		{
			processEntry(ownIt);
			++ownIt;
		}
		else // equal keys
		{
			// own entry overrides master entry
			processEntry(ownIt);
			++masterIt;
			++ownIt;
		}
	}
	while (masterIt != masterEnd)
	{
		processEntry(masterIt);
		++masterIt;
	}
	while (ownIt != ownEnd)
	{
		processEntry(ownIt);
		++ownIt;
	}

	return result;
}

const PackageEntry* Solution::getPackageEntry(const dg::Element* elementPtr) const
{
	auto it = __added_entries->find(elementPtr);
	if (it != __added_entries->end())
	{
		return &it->second;
	}
	if (__master_entries)
	{
		it = __master_entries->find(elementPtr);
		if (it != __master_entries->end())
		{
			if (__removed_entries->find(elementPtr) != __removed_entries->end())
			{
				return NULL;
			}
			return &it->second;
		}
	}

	return NULL; // not found
}

}
}

