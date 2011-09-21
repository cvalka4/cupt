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

#include <cmath>
#include <queue>
#include <algorithm>

#include <common/regex.hpp>

#include <cupt/config.hpp>
#include <cupt/cache.hpp>
#include <cupt/cache/binarypackage.hpp>
#include <cupt/system/state.hpp>

#include <internal/nativeresolver/impl.hpp>
#include <internal/graph.hpp>

namespace cupt {
namespace internal {

using std::queue;

NativeResolverImpl::NativeResolverImpl(const shared_ptr< const Config >& config, const shared_ptr< const Cache >& cache)
	: __config(config), __cache(cache), __score_manager(*config, cache)
{
	__import_installed_versions();
}

void NativeResolverImpl::__import_installed_versions()
{
	auto versions = __cache->getInstalledVersions();
	FORIT(versionIt, versions)
	{
		// just moving versions, don't try to install or remove some dependencies
		const shared_ptr< const BinaryVersion >& version = *versionIt;

		__old_packages[version->packageName] = version;
		__initial_packages[version->packageName].version = version;
	}

	__import_packages_to_reinstall();
}

void NativeResolverImpl::__import_packages_to_reinstall()
{
	bool debugging = __config->getBool("debug::resolver");

	auto reinstallRequiredPackageNames = __cache->getSystemState()->getReinstallRequiredPackageNames();
	FORIT(packageNameIt, reinstallRequiredPackageNames)
	{
		if (debugging)
		{
			debug("the package '%s' needs a reinstall", packageNameIt->c_str());
		}

		// this also involves creating new entry in __initial_packages
		shared_ptr< const BinaryVersion >& targetVersion = __initial_packages[*packageNameIt].version;
		targetVersion.reset(); // removed by default

		// = this package was not installed by resolver
		__manually_modified_package_names.insert(*packageNameIt);
	}
}

void __mydebug_wrapper(const Solution& solution, const string& message)
{
	string levelString(solution.level, ' ');
	debug("%s(%u:%zd) %s", levelString.c_str(), solution.id,
			solution.score, message.c_str());
}

// installs new version, but does not sticks it
bool NativeResolverImpl::__prepare_version_no_stick(
		const shared_ptr< const BinaryVersion >& version,
		dg::InitialPackageEntry& initialPackageEntry)
{
	const string& packageName = version->packageName;
	if (initialPackageEntry.version &&
			initialPackageEntry.version->versionString == version->versionString)
	{
		return true; // there is such version installed already
	}

	if (initialPackageEntry.sticked)
	{
		return false; // package is restricted to be updated
	}

	if (__config->getBool("debug::resolver"))
	{
		debug("install package '%s', version '%s'", packageName.c_str(),
				version->versionString.c_str());
	}
	initialPackageEntry.modified = true;
	initialPackageEntry.version = version;

	return true;
}

void NativeResolverImpl::setAutomaticallyInstalledFlag(const string& packageName, bool flagValue)
{
	if (flagValue)
	{
		__manually_modified_package_names.erase(packageName);
	}
	else
	{
		__manually_modified_package_names.insert(packageName);
	}
}

void NativeResolverImpl::installVersion(const shared_ptr< const BinaryVersion >& version)
{
	const string& packageName = version->packageName;

	dg::InitialPackageEntry& initialPackageEntry = __initial_packages[packageName];
	if (!__prepare_version_no_stick(version, initialPackageEntry))
	{
		fatal("unable to re-schedule package '%s'", packageName.c_str());
	}

	initialPackageEntry.sticked = true;
	__manually_modified_package_names.insert(packageName);
}

void NativeResolverImpl::satisfyRelationExpression(const RelationExpression& relationExpression)
{
	__satisfy_relation_expressions.push_back(relationExpression);
	if (__config->getBool("debug::resolver"))
	{
		debug("strictly satisfying relation '%s'", relationExpression.toString().c_str());
	}
}

void NativeResolverImpl::unsatisfyRelationExpression(const RelationExpression& relationExpression)
{
	__unsatisfy_relation_expressions.push_back(relationExpression);
	if (__config->getBool("debug::resolver"))
	{
		debug("strictly unsatisfying relation '%s'", relationExpression.toString().c_str());
	}
}

void NativeResolverImpl::removePackage(const string& packageName)
{
	dg::InitialPackageEntry& initialPackageEntry = __initial_packages[packageName];
	if (initialPackageEntry.version && initialPackageEntry.sticked)
	{
		fatal("unable to re-schedule package '%s'", packageName.c_str());
	}
	initialPackageEntry.sticked = true;
	initialPackageEntry.modified = true;
	initialPackageEntry.version.reset();
	__manually_modified_package_names.insert(packageName);

	if (__config->getBool("debug::resolver"))
	{
		debug("removing package '%s'", packageName.c_str());
	}
}

void NativeResolverImpl::upgrade()
{
	FORIT(it, __initial_packages)
	{
		dg::InitialPackageEntry& initialPackageEntry = it->second;
		if (!initialPackageEntry.version)
		{
			continue;
		}
		if (initialPackageEntry.sticked)
		{
			continue;
		}

		const string& packageName = it->first;
		auto package = __cache->getBinaryPackage(packageName);

		// if there is original version, then at least one policy version should exist
		auto supposedVersion = static_pointer_cast< const BinaryVersion >
				(__cache->getPolicyVersion(package));
		if (!supposedVersion)
		{
			fatal("internal error: supposed version doesn't exist");
		}

		__prepare_version_no_stick(supposedVersion, initialPackageEntry);
	}
}

struct SolutionScoreLess
{
	bool operator()(const shared_ptr< Solution >& left,
			const shared_ptr< Solution >& right) const
	{
		if (left->score < right->score)
		{
			return true;
		}
		if (left->score > right->score)
		{
			return false;
		}
		return left->id > right->id;
	}
};
typedef set< shared_ptr< Solution >, SolutionScoreLess > SolutionContainer;
typedef std::function< SolutionContainer::iterator (SolutionContainer&) > SolutionChooser;

SolutionContainer::iterator __fair_chooser(SolutionContainer& solutions)
{
	// choose the solution with maximum score
	return --solutions.end();
}

SolutionContainer::iterator __full_chooser(SolutionContainer& solutions)
{
	// defer the decision until all solutions are built
	FORIT(solutionIt, solutions)
	{
		if (! (*solutionIt)->finished)
		{
			return solutionIt;
		}
	}

	// heh, the whole solution tree has been already built?.. ok, let's choose
	// the best solution
	return __fair_chooser(solutions);
}

bool NativeResolverImpl::__is_candidate_for_auto_removal(const dg::Element* elementPtr,
		const std::function< bool (const string&) > isNeverAutoRemove,
		bool canAutoremove)
{
	auto versionVertex = dynamic_cast< const dg::VersionVertex* >(elementPtr);
	if (!versionVertex)
	{
		return false;
	}

	const string& packageName = versionVertex->getPackageName();
	const shared_ptr< const BinaryVersion >& version = versionVertex->version;

	if (packageName == __dummy_package_name)
	{
		return false;
	}
	if (!version)
	{
		return false;
	}
	{ // checking was the package initially requested
		auto initialPackageIt = __initial_packages.find(packageName);
		if (initialPackageIt != __initial_packages.end() && initialPackageIt->second.sticked)
		{
			return false;
		}
	}
	if (version->essential)
	{
		return false;
	}

	auto canAutoremoveThisPackage = canAutoremove && __cache->isAutomaticallyInstalled(packageName);
	if (__old_packages.count(packageName) && !canAutoremoveThisPackage)
	{
		return false;
	}
	if (isNeverAutoRemove(packageName))
	{
		return false;
	}
	return true;
}

void NativeResolverImpl::__clean_automatically_installed(Solution& solution)
{
	vector< sregex > neverAutoRemoveRegexes;
	{
		auto neverAutoRemoveRegexStrings = __config->getList("apt::neverautoremove");

		FORIT(regexStringIt, neverAutoRemoveRegexStrings)
		{
			try
			{
				neverAutoRemoveRegexes.push_back(sregex::compile(*regexStringIt));
			}
			catch (regex_error&)
			{
				fatal("invalid regular expression '%s'", regexStringIt->c_str());
			}
		}
	}

	smatch m;
	auto isNeverAutoRemove = [&neverAutoRemoveRegexes, &m](const string& packageName) -> bool
	{
		FORIT(regexIt, neverAutoRemoveRegexes)
		{
			if (regex_match(packageName, m, *regexIt))
			{
				return true;
			}
		}
		return false;
	};

	auto canAutoremove = __config->getBool("cupt::resolver::auto-remove");

	Graph< const dg::Element* > dependencyGraph;
	auto mainVertexPtr = dependencyGraph.addVertex(NULL);
	const set< const dg::Element* >& vertices = dependencyGraph.getVertices();
	{ // building dependency graph
		auto elementPtrs = solution.getElements();
		FORIT(elementPtrIt, elementPtrs)
		{
			dependencyGraph.addVertex(*elementPtrIt);
		}
		FORIT(elementPtrIt, vertices)
		{
			if (!*elementPtrIt)
			{
				continue; // main vertex
			}
			const GraphCessorListType& successorElementPtrs =
					__solution_storage->getSuccessorElements(*elementPtrIt);
			FORIT(successorElementPtrIt, successorElementPtrs)
			{
				if ((*successorElementPtrIt)->isAnti())
				{
					continue;
				}
				const GraphCessorListType& successorSuccessorElementPtrs =
						__solution_storage->getSuccessorElements(*successorElementPtrIt);
				FORIT(successorSuccessorElementPtrIt, successorSuccessorElementPtrs)
				{
					auto it = vertices.find(*successorSuccessorElementPtrIt);
					if (it != vertices.end())
					{
						// found, add the edge
						dependencyGraph.addEdgeFromPointers(&*elementPtrIt, &*it);
					}
				}
			}

			if (!__is_candidate_for_auto_removal(*elementPtrIt, isNeverAutoRemove, canAutoremove))
			{
				dependencyGraph.addEdgeFromPointers(mainVertexPtr, &*elementPtrIt);
			}
		}
	}

	{ // looping through the candidates
		bool debugging = __config->getBool("debug::resolver");

		auto reachableElementPtrPtrs = dependencyGraph.getReachableFrom(*mainVertexPtr);

		FORIT(elementPtrIt, vertices)
		{
			if (!reachableElementPtrPtrs.count(&*elementPtrIt))
			{
				PackageEntry packageEntry;
				packageEntry.autoremoved = true;
				auto emptyElementPtr = __solution_storage->getCorrespondingEmptyElement(*elementPtrIt);

				if (debugging)
				{
					__mydebug_wrapper(solution, sf("auto-removed '%s'", (*elementPtrIt)->toString().c_str()));
				}
				__solution_storage->setPackageEntry(solution, emptyElementPtr,
						std::move(packageEntry), *elementPtrIt);
			}
		}
	}
}

SolutionChooser __select_solution_chooser(const Config& config)
{
	SolutionChooser result;

	auto resolverType = config.getString("cupt::resolver::type");
	if (resolverType == "fair")
	{
		result = __fair_chooser;
	}
	else if (resolverType == "full")
	{
		result = __full_chooser;
	}
	else
	{
		fatal("wrong resolver type '%s'", resolverType.c_str());
	}

	return result;
}

void NativeResolverImpl::__require_strict_relation_expressions()
{
	// "installing" virtual package, which will be used for strict '(un)satisfy' requests
	shared_ptr< BinaryVersion > version(new BinaryVersion);

	version->packageName = __dummy_package_name;
	version->sourcePackageName = __dummy_package_name;
	version->versionString = "";
	version->relations[BinaryVersion::RelationTypes::Depends] = __satisfy_relation_expressions;
	version->relations[BinaryVersion::RelationTypes::Breaks] = __unsatisfy_relation_expressions;

	dg::InitialPackageEntry initialPackageEntry;
	initialPackageEntry.version = version;
	initialPackageEntry.sticked = true;
	__initial_packages[__dummy_package_name] = initialPackageEntry;
}

/* __pre_apply_action only prints debug info and changes level/score of the
   solution, not modifying packages in it, economing RAM and CPU,
   __post_apply_action will perform actual changes when the solution is picked up
   by resolver */

void NativeResolverImpl::__pre_apply_action(const Solution& originalSolution,
		Solution& solution, unique_ptr< Action >&& actionToApply)
{
	if (originalSolution.finished)
	{
		fatal("internal error: an attempt to make changes to already finished solution");
	}

	auto oldElementPtr = actionToApply->oldElementPtr;
	auto newElementPtr = actionToApply->newElementPtr;
	const ScoreChange& profit = actionToApply->profit;

	if (__config->getBool("debug::resolver"))
	{
		auto message = sf("-> (%u,Δ:[%s]) trying: '%s' -> '%s'",
				solution.id, __score_manager.getScoreChangeString(profit).c_str(),
				oldElementPtr ? oldElementPtr->toString().c_str() : "",
				newElementPtr->toString().c_str());
		__mydebug_wrapper(originalSolution, message);
	}

	solution.level += 1;
	solution.score += __score_manager.getScoreChangeValue(profit);

	solution.pendingAction = std::forward< unique_ptr< Action >&& >(actionToApply);
}

void NativeResolverImpl::__calculate_profits(vector< unique_ptr< Action > >& actions) const
{
	auto getVersion = [](const dg::Element* elementPtr) -> shared_ptr< const BinaryVersion >
	{
		static shared_ptr< const BinaryVersion > emptyVersion;
		if (!elementPtr)
		{
			return emptyVersion;
		}
		auto versionVertex = dynamic_cast< const dg::VersionVertex* >(elementPtr);
		if (!versionVertex)
		{
			return emptyVersion;
		}
		return versionVertex->version;
	};

	size_t position = 0;
	FORIT(actionIt, actions)
	{
		Action& action = **actionIt;

		switch (action.newElementPtr->getUnsatisfiedType())
		{
			case dg::Unsatisfied::None:
				action.profit = __score_manager.getVersionScoreChange(
						getVersion(action.oldElementPtr), getVersion(action.newElementPtr));
				break;
			case dg::Unsatisfied::Recommends:
				action.profit = __score_manager.getUnsatisfiedRecommendsScoreChange();
				break;
			case dg::Unsatisfied::Suggests:
				action.profit = __score_manager.getUnsatisfiedSuggestsScoreChange();
				break;
			case dg::Unsatisfied::Sync:
				action.profit = __score_manager.getUnsatisfiedSynchronizationScoreChange();
				break;
		}
		action.profit.setPosition(position);
		++position;
	}
}

void NativeResolverImpl::__pre_apply_actions_to_solution_tree(
		std::function< void (const shared_ptr< Solution >&) > callback,
		const shared_ptr< Solution >& currentSolution, vector< unique_ptr< Action > >& actions)
{
	// sort them by "rank", from more good to more bad
	std::stable_sort(actions.begin(), actions.end(),
			[this](const unique_ptr< Action >& left, const unique_ptr< Action >& right) -> bool
			{
				return this->__score_manager.getScoreChangeValue(right->profit)
						< this->__score_manager.getScoreChangeValue(left->profit);
			});

	// fork the solution entry and apply all the solutions by one
	FORIT(actionIt, actions)
	{
		// clone the current stack to form a new one
		auto clonedSolution = __solution_storage->cloneSolution(currentSolution);

		// apply the solution
		__pre_apply_action(*currentSolution, *clonedSolution, std::move(*actionIt));

		callback(clonedSolution);
	}
}

void __erase_worst_solutions(SolutionContainer& solutions,
		size_t maxSolutionCount, bool debugging, bool& thereWereDrops)
{
	// don't allow solution tree to grow unstoppably
	while (solutions.size() > maxSolutionCount)
	{
		// drop the worst solution
		auto worstSolutionIt = solutions.begin();
		if (debugging)
		{
			__mydebug_wrapper(**worstSolutionIt, "dropped");
		}
		solutions.erase(worstSolutionIt);
		if (!thereWereDrops)
		{
			thereWereDrops = true;
			warn("some solutions were dropped, you may want to increase the value of the '%s' option",
					"cupt::resolver::max-solution-count");
		}
	}
}

void NativeResolverImpl::__post_apply_action(Solution& solution)
{
	if (!solution.pendingAction)
	{
		fatal("internal error: __post_apply_action: no action to apply");
	}
	const Action& action = *(static_cast< const Action* >(solution.pendingAction.get()));

	PackageEntry packageEntry;
	packageEntry.sticked = true;
	packageEntry.introducedBy = action.introducedBy;
	__solution_storage->setPackageEntry(solution, action.newElementPtr,
			std::move(packageEntry), action.oldElementPtr);
	solution.insertedElementPtrs.push_back(action.newElementPtr);
	__validate_changed_package(solution, action.oldElementPtr,
			action.newElementPtr, action.brokenElementPriority + 1);

	solution.pendingAction.reset();
}

bool NativeResolverImpl::__makes_sense_to_modify_package(const Solution& solution,
		const dg::Element* candidateElementPtr, const dg::Element* brokenElementPtr,
		bool debugging)
{
	/* we check only successors with the same or bigger priority than
	   currently broken one */
	auto brokenElementTypePriority = brokenElementPtr->getTypePriority();

	__solution_storage->unfoldElement(candidateElementPtr);

	const GraphCessorListType& successorElementPtrs =
			__solution_storage->getSuccessorElements(candidateElementPtr);
	FORIT(successorElementPtrIt, successorElementPtrs)
	{
		if ((*successorElementPtrIt)->getTypePriority() < brokenElementTypePriority)
		{
			continue;
		}
		if (*successorElementPtrIt == brokenElementPtr)
		{
			if (debugging)
			{
				__mydebug_wrapper(solution, sf(
						"not considering %s: it has the same problem",
						candidateElementPtr->toString().c_str()));
			}
			return false;
		}
	}

	// let's try even harder to find if this candidate is really appropriate for us
	const GraphCessorListType& brokenElementSuccessorElementPtrs =
			__solution_storage->getSuccessorElements(brokenElementPtr);
	FORIT(successorElementPtrIt, successorElementPtrs)
	{
		if ((*successorElementPtrIt)->getTypePriority() < brokenElementTypePriority)
		{
			continue;
		}
		/* if any of such successors gives us equal or less "space" in
		   terms of satisfying elements, the version won't be accepted as a
		   resolution */
		const GraphCessorListType& successorElementSuccessorElementPtrs =
				__solution_storage->getSuccessorElements(*successorElementPtrIt);

		bool isMoreWide = false;
		FORIT(elementPtrIt, successorElementSuccessorElementPtrs)
		{
			bool notFound = (std::find(brokenElementSuccessorElementPtrs.begin(),
					brokenElementSuccessorElementPtrs.end(), *elementPtrIt)
					== brokenElementSuccessorElementPtrs.end());

			if (notFound)
			{
				// more wide relation, can't say nothing bad with it at time being
				isMoreWide = true;
				break;
			}
		}

		if (!isMoreWide)
		{
			if (debugging)
			{
				__mydebug_wrapper(solution, sf(
						"not considering %s: it contains equal or less wide relation expression '%s'",
						candidateElementPtr->toString().c_str(), (*successorElementPtrIt)->toString().c_str()));
			}
			return false;
		}
	}

	return true;
}

void NativeResolverImpl::__add_actions_to_modify_package_entry(
		vector< unique_ptr< Action > >& actions, const Solution& solution,
		const dg::Element* versionElementPtr, const dg::Element* brokenElementPtr,
		bool debugging)
{
	if (solution.getPackageEntry(versionElementPtr)->sticked)
	{
		return;
	}

	const forward_list< const dg::Element* >& conflictingElementPtrs =
			__solution_storage->getConflictingElements(versionElementPtr);
	FORIT(conflictingElementPtrIt, conflictingElementPtrs)
	{
		if (*conflictingElementPtrIt == versionElementPtr)
		{
			continue;
		}
		if (__makes_sense_to_modify_package(solution, *conflictingElementPtrIt,
				brokenElementPtr, debugging))
		{
			// other version seems to be ok
			unique_ptr< Action > action(new Action);
			action->oldElementPtr = versionElementPtr;
			action->newElementPtr = *conflictingElementPtrIt;

			actions.push_back(std::move(action));
		}
	}
}

void NativeResolverImpl::__add_actions_to_fix_dependency(vector< unique_ptr< Action > >& actions,
		const Solution& solution, const dg::Element* brokenElementPtr)
{
	const GraphCessorListType& successorElementPtrs =
			__solution_storage->getSuccessorElements(brokenElementPtr);
	// install one of versions package needs
	FORIT(successorElementPtrIt, successorElementPtrs)
	{
		const dg::Element* conflictingElementPtr;
		if (__solution_storage->simulateSetPackageEntry(solution, *successorElementPtrIt, &conflictingElementPtr))
		{
			unique_ptr< Action > action(new Action);
			action->oldElementPtr = conflictingElementPtr;
			action->newElementPtr = *successorElementPtrIt;

			actions.push_back(std::move(action));
		}
	}
}

Resolver::UserAnswer::Type NativeResolverImpl::__propose_solution(
		const Solution& solution, Resolver::CallbackType callback, bool trackReasons)
{
	static const Resolver::SuggestedPackage emptySuggestedPackage;
	static const shared_ptr< system::Resolver::UserReason >
			userReason(new system::Resolver::UserReason);
	static const shared_ptr< const Reason > autoRemovalReason(new AutoRemovalReason);


	// build "user-frienly" version of solution
	Resolver::Offer offer;
	Resolver::SuggestedPackages& suggestedPackages = offer.suggestedPackages;

	auto elementPtrs = solution.getElements();
	FORIT(elementPtrIt, elementPtrs)
	{
		auto vertex = dynamic_cast< const dg::VersionVertex* >(*elementPtrIt);
		if (vertex)
		{
			const string& packageName = vertex->getPackageName();
			if (packageName == __dummy_package_name)
			{
				continue;
			}

			// iterator of inserted element
			auto it = suggestedPackages.insert(make_pair(packageName, emptySuggestedPackage)).first;
			Resolver::SuggestedPackage& suggestedPackage = it->second;
			suggestedPackage.version = vertex->version;

			if (trackReasons)
			{
				auto packageEntryPtr = solution.getPackageEntry(*elementPtrIt);
				if (!packageEntryPtr->introducedBy.empty())
				{
					suggestedPackage.reasons.push_back(packageEntryPtr->introducedBy.getReason());
				}
				if (packageEntryPtr->autoremoved)
				{
					suggestedPackage.reasons.push_back(autoRemovalReason);
				}
				auto initialPackageIt = __initial_packages.find(packageName);
				if (initialPackageIt != __initial_packages.end() && initialPackageIt->second.modified)
				{
					suggestedPackage.reasons.push_back(userReason);
				}
			}
			suggestedPackage.manuallySelected = __manually_modified_package_names.count(packageName);
		}
		else
		{
			// non-version vertex - unsatisfied one
			const GraphCessorListType& predecessors =
					__solution_storage->getPredecessorElements(*elementPtrIt);
			FORIT(predecessorIt, predecessors)
			{
				const GraphCessorListType& affectedVersionElements =
						__solution_storage->getPredecessorElements(*predecessorIt);
				FORIT(affectedVersionElementIt, affectedVersionElements)
				{
					if (solution.getPackageEntry(*affectedVersionElementIt))
					{
						offer.unresolvedProblems.push_back(
								(*predecessorIt)->getReason(**affectedVersionElementIt));
					}
				}
			}
		}
	}

	// suggest found solution
	bool debugging = __config->getBool("debug::resolver");
	if (debugging)
	{
		__mydebug_wrapper(solution, "proposing this solution");
	}

	auto userAnswer = callback(offer);
	if (debugging)
	{
		if (userAnswer == Resolver::UserAnswer::Accept)
		{
			__mydebug_wrapper(solution, "accepted");
		}
		else if (userAnswer == Resolver::UserAnswer::Decline)
		{
			__mydebug_wrapper(solution, "declined");
		}
	}

	return userAnswer;
}

void NativeResolverImpl::__generate_possible_actions(vector< unique_ptr< Action > >* possibleActionsPtr,
		const Solution& solution, const dg::Element* versionElementPtr,
		const dg::Element* brokenElementPtr, bool debugging)
{
	__add_actions_to_fix_dependency(*possibleActionsPtr, solution, brokenElementPtr);
	__add_actions_to_modify_package_entry(*possibleActionsPtr, solution,
			versionElementPtr, brokenElementPtr, debugging);
}

void NativeResolverImpl::__validate_element(
		Solution& solution, const dg::Element* elementPtr, size_t priority)
{
	const GraphCessorListType& successorElementPtrs =
			__solution_storage->getSuccessorElements(elementPtr);
	forward_list< PackageEntry::BrokenSuccessor > brokenSuccessors;
	FORIT(successorElementPtrIt, successorElementPtrs)
	{
		if (!__solution_storage->verifyElement(solution, *successorElementPtrIt))
		{
			brokenSuccessors.push_front(
					PackageEntry::BrokenSuccessor(*successorElementPtrIt, priority));
		}
	}
	if (!brokenSuccessors.empty())
	{
		PackageEntry packageEntry = *solution.getPackageEntry(elementPtr);
		packageEntry.brokenSuccessors.swap(brokenSuccessors);
		__solution_storage->setPackageEntry(solution, elementPtr,
				std::move(packageEntry), NULL);
	}
}

void NativeResolverImpl::__initial_validate_pass(Solution& solution)
{
	auto elementPtrs = solution.getElements();
	FORIT(elementPtrIt, elementPtrs)
	{
		__validate_element(solution, *elementPtrIt, 0u);
	}
}

void NativeResolverImpl::__final_verify_solution(const Solution& solution)
{
	auto elementPtrs = solution.getElements();
	FORIT(elementPtrIt, elementPtrs)
	{
		const GraphCessorListType& successorElementPtrs =
				__solution_storage->getSuccessorElements(*elementPtrIt);
		FORIT(successorElementPtrIt, successorElementPtrs)
		{
			if (!__solution_storage->verifyElement(solution, *successorElementPtrIt))
			{
				fatal("internal error: final solution check failed: solution '%u', version '%s', problem '%s'",
						solution.id, (*elementPtrIt)->toString().c_str(),
						(*successorElementPtrIt)->toString().c_str());
			}
		}
	}
}

void NativeResolverImpl::__validate_changed_package(Solution& solution,
		const dg::Element* oldElementPtr, const dg::Element* newElementPtr,
		size_t priority)
{
	__validate_element(solution, newElementPtr, priority);

	if (oldElementPtr)
	{ // invalidate those which depend on the old element
		const GraphCessorListType& predecessors =
				__solution_storage->getPredecessorElements(oldElementPtr);
		FORIT(predecessorElementPtrIt, predecessors)
		{
			if (!__solution_storage->verifyElement(solution, *predecessorElementPtrIt))
			{
				const GraphCessorListType& dependentVersionElementPtrs =
						__solution_storage->getPredecessorElements(*predecessorElementPtrIt);
				FORIT(versionElementPtrIt, dependentVersionElementPtrs)
				{
					auto packageEntryPtr = solution.getPackageEntry(*versionElementPtrIt);
					if (!packageEntryPtr)
					{
						continue; // no such element in this solution
					}
					// here we assume packageEntry.brokenSuccessors doesn't
					// contain predecessorElementPtr, since as old element was
					// present, predecessorElementPtr was not broken
					PackageEntry packageEntry = *packageEntryPtr;
					packageEntry.brokenSuccessors.push_front(
							PackageEntry::BrokenSuccessor(*predecessorElementPtrIt, priority));
					__solution_storage->setPackageEntry(solution, *versionElementPtrIt,
							std::move(packageEntry), NULL);
				}
			}
		}
	}
	{ // validate those which depend on the new element
		const GraphCessorListType& predecessors =
				__solution_storage->getPredecessorElements(newElementPtr);
		FORIT(predecessorElementPtrIt, predecessors)
		{
			const GraphCessorListType& dependentVersionElementPtrs =
					__solution_storage->getPredecessorElements(*predecessorElementPtrIt);
			FORIT(versionElementPtrIt, dependentVersionElementPtrs)
			{
				auto packageEntryPtr = solution.getPackageEntry(*versionElementPtrIt);
				if (!packageEntryPtr)
				{
					continue; // no such element in this solution
				}
				FORIT(existingBrokenSuccessorIt, packageEntryPtr->brokenSuccessors)
				{
					if (existingBrokenSuccessorIt->elementPtr == *predecessorElementPtrIt)
					{
						PackageEntry packageEntry = *packageEntryPtr;
						packageEntry.brokenSuccessors.remove_if(
								[predecessorElementPtrIt](const PackageEntry::BrokenSuccessor& brokenSuccessor)
								{
									return brokenSuccessor.elementPtr == *predecessorElementPtrIt;
								});
						__solution_storage->setPackageEntry(solution, *versionElementPtrIt,
								std::move(packageEntry), NULL);
						break;
					}
				}
			}
		}
	}
}

pair< const dg::Element*, PackageEntry::BrokenSuccessor > __get_broken_pair(
		const Solution& solution, const map< const dg::Element*, size_t >& failCounts)
{
	typedef pair< const dg::Element*, PackageEntry::BrokenSuccessor > BrokenPairType;
	auto brokenPairs = solution.getBrokenPairs();
	if (brokenPairs.empty())
	{
		static BrokenPairType noPair(NULL, PackageEntry::BrokenSuccessor());
		return noPair;
	}
	auto failValue = [&failCounts](const dg::Element* e) -> size_t
	{
		auto it = failCounts.find(e);
		return it != failCounts.end() ? it->second : 0u;
	};
	return *std::max_element(brokenPairs.begin(), brokenPairs.end(),
			[failValue](const BrokenPairType& left, const BrokenPairType& right)
			{
				auto leftTypePriority = left.second.elementPtr->getTypePriority();
				auto rightTypePriority = right.second.elementPtr->getTypePriority();
				if (leftTypePriority < rightTypePriority)
				{
					return true;
				}
				if (leftTypePriority > rightTypePriority)
				{
					return false;
				}

				if (left.second.priority < right.second.priority)
				{
					return true;
				}
				if (left.second.priority > right.second.priority)
				{
					return false;
				}

				auto leftFailValue = failValue(left.second.elementPtr);
				auto rightFailValue = failValue(right.second.elementPtr);
				if (leftFailValue < rightFailValue)
				{
					return true;
				}
				if (leftFailValue > rightFailValue)
				{
					return false;
				}

				return static_cast< const dg::VersionVertex* >(left.first)->getPackageName() >
						static_cast< const dg::VersionVertex* >(right.first)->getPackageName();
			});
}

bool NativeResolverImpl::resolve(Resolver::CallbackType callback)
{
	auto solutionChooser = __select_solution_chooser(*__config);

	const bool debugging = __config->getBool("debug::resolver");
	const bool trackReasons = __config->getBool("cupt::resolver::track-reasons");
	const size_t maxSolutionCount = __config->getInteger("cupt::resolver::max-solution-count");
	bool thereWereSolutionsDropped = false;

	if (debugging)
	{
		debug("started resolving");
	}
	__require_strict_relation_expressions();

	__any_solution_was_found = false;
	__decision_fail_tree.clear();

	shared_ptr< Solution > initialSolution(new Solution);
	__solution_storage.reset(new SolutionStorage(*__config, *__cache));
	__solution_storage->prepareForResolving(*initialSolution, __old_packages, __initial_packages);
	__initial_validate_pass(*initialSolution);

	SolutionContainer solutions = { initialSolution };

	// for each package entry 'count' will contain the number of failures
	// during processing these packages
	map< const dg::Element*, size_t > failCounts;

	bool checkFailed;

	while (!solutions.empty())
	{
		vector< unique_ptr< Action > > possibleActions;

		// choosing the solution to process
		shared_ptr< Solution > currentSolution;
		{
			auto currentSolutionIt = solutionChooser(solutions);
			currentSolution = *currentSolutionIt;
			solutions.erase(currentSolutionIt);
		}

		if (currentSolution->pendingAction)
		{
			currentSolution->prepare();
			__post_apply_action(*currentSolution);
		}

		// for the speed reasons, we will correct one-solution problems directly in MAIN_LOOP
		// so, when an intermediate problem was solved, maybe it breaks packages
		// we have checked earlier in the loop, so we schedule a recheck
		//
		// once two or more solutions are available, loop will be ended immediately
		bool recheckNeeded = true;
		while (recheckNeeded)
		{
			recheckNeeded = false;
			checkFailed = false;

			const dg::Element* versionElementPtr;
			PackageEntry::BrokenSuccessor brokenSuccessor;
			{
				auto brokenPair = __get_broken_pair(*currentSolution, failCounts);
				versionElementPtr = brokenPair.first;
				if (!versionElementPtr)
				{
					break;
				}
				brokenSuccessor = brokenPair.second;
			}
			checkFailed = true;

			if (debugging)
			{
				auto message = sf("problem (%zu:%zu): %s: %s",
						brokenSuccessor.elementPtr->getTypePriority(), brokenSuccessor.priority,
						versionElementPtr->toString().c_str(),
						brokenSuccessor.elementPtr->toString().c_str());
				__mydebug_wrapper(*currentSolution, message);
			}
			__generate_possible_actions(&possibleActions, *currentSolution,
					versionElementPtr, brokenSuccessor.elementPtr, debugging);

			{
				PackageEntry::IntroducedBy ourIntroducedBy;
				ourIntroducedBy.versionElementPtr = versionElementPtr;
				ourIntroducedBy.brokenElementPtr = brokenSuccessor.elementPtr;

				if (possibleActions.empty() && !__any_solution_was_found)
				{
					__decision_fail_tree.addFailedSolution(*__solution_storage,
							*currentSolution, ourIntroducedBy);
				}
				else
				{
					FORIT(actionIt, possibleActions)
					{
						(*actionIt)->introducedBy = ourIntroducedBy;
					}
				}
			}

			FORIT(actionIt, possibleActions)
			{
				(*actionIt)->brokenElementPriority = brokenSuccessor.priority;
			}

			// mark package as failed one more time
			failCounts[brokenSuccessor.elementPtr] += 1;

			if (possibleActions.size() == 1)
			{
				__calculate_profits(possibleActions);
				__pre_apply_action(*currentSolution, *currentSolution, std::move(possibleActions[0]));
				__post_apply_action(*currentSolution);
				possibleActions.clear();

				recheckNeeded = true;
			}
		}

		if (!checkFailed)
		{
			// if the solution was only just finished
			if (!currentSolution->finished)
			{
				if (debugging)
				{
					__mydebug_wrapper(*currentSolution, "finished");
				}
				currentSolution->finished = 1;
			}
			if (!__any_solution_was_found)
			{
				__any_solution_was_found = true;
				__decision_fail_tree.clear(); // no need to store this tree anymore
			}

			// resolver can refuse the solution
			solutions.insert(currentSolution);
			auto newSelectedSolutionIt = solutionChooser(solutions);
			if (*newSelectedSolutionIt != currentSolution)
			{
				continue; // ok, process other solution
			}
			solutions.erase(newSelectedSolutionIt);

			// clean up automatically installed by resolver and now unneeded packages
			__clean_automatically_installed(*currentSolution);

			__final_verify_solution(*currentSolution);

			auto userAnswer = __propose_solution(*currentSolution, callback, trackReasons);
			switch (userAnswer)
			{
				case Resolver::UserAnswer::Accept:
					// yeah, this is end of our tortures
					return true;
				case Resolver::UserAnswer::Abandon:
					// user has selected abandoning all further efforts
					return false;
				case Resolver::UserAnswer::Decline:
					; // caller hasn't accepted this solution, well, go next...
			}
		}
		else
		{
			if (!possibleActions.empty())
			{
				__calculate_profits(possibleActions);

				auto callback = [&solutions](const shared_ptr< Solution >& solution)
				{
					solutions.insert(solution);
				};
				__pre_apply_actions_to_solution_tree(callback, currentSolution, possibleActions);
			}
			else
			{
				if (debugging)
				{
					__mydebug_wrapper(*currentSolution, "no solutions");
				}
			}

			if (!possibleActions.empty())
			{
				// some new solutions were added
				__erase_worst_solutions(solutions, maxSolutionCount, debugging, thereWereSolutionsDropped);
			}
		}
	}
	if (!__any_solution_was_found)
	{
		// no solutions pending, we have a great fail
		fatal("unable to resolve dependencies, because of:\n\n%s",
				__decision_fail_tree.toString().c_str());
	}
	return false;
}

const string NativeResolverImpl::__dummy_package_name = "dummy_package_name";

}
}

