#***************************************************************************
#*   Copyright (C) 2008-2009 by Eugene V. Lyubimkin                        *
#*                                                                         *
#*   This program is free software; you can redistribute it and/or modify  *
#*   it under the terms of the GNU General Public License                  *
#*   (version 3 or above) as published by the Free Software Foundation.    *
#*                                                                         *
#*   This program is distributed in the hope that it will be useful,       *
#*   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
#*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
#*   GNU General Public License for more details.                          *
#*                                                                         *
#*   You should have received a copy of the GNU GPL                        *
#*   along with this program; if not, write to the                         *
#*   Free Software Foundation, Inc.,                                       *
#*   51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA               *
#*                                                                         *
#*   This program is free software; you can redistribute it and/or modify  *
#*   it under the terms of the Artistic License, which comes with Perl     *
#***************************************************************************
package Cupt::System::Resolvers::Native;

=head1 NAME

Cupt::System::Resolvers::Native - native (built-in) dependency problem resolver for Cupt

=cut

use 5.10.0;
use strict;
use warnings;

use base qw(Cupt::System::Resolver);

use List::Util qw(reduce first);
use List::MoreUtils 0.23 qw(any none);

use Cupt::Core;
use Cupt::Cache::Relation qw(stringify_relation_expression);
use Cupt::System::Resolvers::Native::PackageEntry;
use Cupt::System::Resolvers::Native::Solution;
use Cupt::Graph;

our $dummy_package_name = '<satisfy>';

use Cupt::LValueFields qw(2 _old_solution _initial_solution _next_free_solution_identifier
		_strict_satisfy_relation_expressions _strict_unsatisfy_relation_expressions);

sub new {
	my $class = shift;
	my $self = bless [] => $class;
	$self->SUPER::new(@_);

	$self->_old_solution = Cupt::System::Resolvers::Native::Solution->new(
			$self->cache, $self->_get_dependency_groups(), {});
	$self->_next_free_solution_identifier = 1;
	$self->_strict_satisfy_relation_expressions = [];
	$self->_strict_unsatisfy_relation_expressions = [];

	return $self;
}

sub _get_dependency_groups {
	my ($self) = @_;

	my @dependency_groups;
	push @dependency_groups, { 'name' => 'pre_depends', 'target' => 'normal', 'index' => 0 };
	push @dependency_groups, { 'name' => 'depends', 'target' => 'normal', 'index' => 1 };
	push @dependency_groups, { 'name' => 'conflicts', 'target' => 'anti', 'index' => 2 };
	push @dependency_groups, { 'name' => 'breaks', 'target' => 'anti', 'index' => 3 };
	if ($self->config->get_bool('cupt::resolver::keep-recommends')) {
		push @dependency_groups, { 'name' => 'recommends', 'target' => 'normal', 'index' => 4 };
	}
	if ($self->config->get_bool('cupt::resolver::keep-suggests')) {
		push @dependency_groups, { 'name' => 'suggests', 'target' => 'normal', 'index' => 5 };
	}

	return \@dependency_groups;
}

sub __mydebug_wrapper {
	my ($solution, @rest) = @_;

	my $level = $solution->level;
	my $identifier = $solution->identifier;
	my $score_string = sprintf '%.1f', $solution->score;
	mydebug(' ' x $level . "($identifier:$score_string) @rest");

	return;
}

sub import_installed_versions ($$) {
	my ($self, $ref_versions) = @_;

	# '_initial_solution' will be modified, leave '_old_solution' as original system state
	foreach my $version (@$ref_versions) {
		# just moving versions, don't try to install or remove some dependencies
		my $package_name = $version->package_name;
		my $package_entry = $self->_old_solution->set_package_entry($package_name);
		$package_entry->version = $version;
		$package_entry->installed = 1;
	}
	$self->_initial_solution = $self->_old_solution->clone();
	$self->_initial_solution->identifier = -1;
	$self->_initial_solution->prepare();

	return;
}

sub _related_binary_package_names ($$$) {
	my ($self, $solution, $version) = @_;

	my @result;

	my $package_name = $version->package_name;
	my $source_package_name = $version->source_package_name;

	my @possible_related_package_names;

	my $source_package = $self->cache->get_source_package($source_package_name);
	if (defined $source_package) {
		my $source_version = $source_package->get_specific_version($version->source_version_string);
		if (defined $source_version) {
			@possible_related_package_names = grep { defined $solution->get_package_entry($_) }
					@{$source_version->binary_package_names};
		}
	}
	if (not scalar @possible_related_package_names) {
		@possible_related_package_names = $solution->get_package_names();
	}

	foreach my $other_package_name (@possible_related_package_names) {
		my $other_version = $solution->get_package_entry($other_package_name)->version;
		next if not defined $other_version;
		next if $other_version->source_package_name ne $source_package_name;
		next if $other_version->package_name eq $package_name;
		push @result, $other_version->package_name;
	}

	return @result;
}

sub _get_package_version_by_source_version_string ($$) {
	my ($self, $package_name, $source_version_string) = @_;

	foreach my $version (@{$self->cache->get_binary_package($package_name)->get_versions()}) {
		if ($version->source_version_string eq $source_version_string) {
			return $version;
		}
	}

	return undef;
}

sub _get_unsynchronizeable_related_package_names {
	my ($self, $solution, $version) = @_;

	my $source_package_name = $version->source_package_name;
	if (any { $source_package_name =~ m/^$_$/ }
		$self->config->get_list('cupt::resolver::synchronize-source-versions::exceptions'))
	{
		return ();
	}

	my $package_name = $version->package_name;
	my @related_package_names = $self->_related_binary_package_names($solution, $version);
	my $source_version_string = $version->source_version_string;

	my @result;

	foreach my $other_package_name (@related_package_names) {
		my $other_package_entry = $solution->get_package_entry($other_package_name);
		my $other_version = $other_package_entry->version;
		if ($other_version->source_version_string eq $source_version_string)
		{
			# no update needed
			next;
		}

		if ($other_package_entry->stick or
			not defined $self->_get_package_version_by_source_version_string(
					$other_package_name, $source_version_string))
		{
			# cannot update the package
			push @result, $other_package_name;
		}
	}

	return @result;
}

sub _related_packages_can_be_synchronized ($$) {
	my ($self, $solution, $version) = @_;

	my @unsynchronizeable_package_names = $self->_get_unsynchronizeable_related_package_names(
			$solution, $version);
	return (scalar @unsynchronizeable_package_names == 0);
}

sub _synchronize_related_packages ($$$$$) {
	# $stick - boolean
	my ($self, $solution, $version, $stick) = @_;

	my @related_package_names = $self->_related_binary_package_names($solution, $version);
	my $source_version_string = $version->source_version_string;
	my $package_name = $version->package_name;

	foreach my $other_package_name (@related_package_names) {
		my $package_entry = $solution->get_package_entry($other_package_name);
		next if $package_entry->stick;
		next if $package_entry->version->source_version_string eq $source_version_string;
		my $candidate_version = $self->_get_package_version_by_source_version_string(
				$other_package_name, $source_version_string);
		next if not defined $candidate_version;

		$package_entry = $solution->set_package_entry($other_package_name);

		$package_entry->version = $candidate_version;
		$package_entry->stick = $stick;
		if ($self->config->get_bool('debug::resolver')) {
			__mydebug_wrapper($solution, "synchronizing package '$other_package_name' with package '$package_name'");
		}
		if ($self->config->get_bool('cupt::resolver::track-reasons')) {
			push @{$package_entry->reasons}, [ 'sync', $package_name ];
		}
	}

	# ok, no errors
	return 1;
}

# installs new version, schedules new dependencies, but not sticks it
sub _install_version_no_stick ($$$) {
	my ($self, $version, $reason) = @_;

	my $package_name = $version->package_name;
	my $package_entry = $self->_initial_solution->get_package_entry($package_name);
	if (not defined $package_entry) {
		$package_entry = $self->_initial_solution->set_package_entry($package_name);
	}

	# maybe nothing changed?
	my $current_version = $package_entry->version;
	if (defined $current_version && $current_version->version_string eq $version->version_string)
	{
		return '';
	}

	if ($package_entry->stick) {
		# package is restricted to be updated
		return sprintf __("unable to re-schedule package '%s'"), $package_name;
	}

	my $o_synchronize_source_versions = $self->config->get_bool('cupt::resolver::synchronize-source-versions');
	if ($o_synchronize_source_versions eq 'hard') {
		# need to check is the whole operation doable
		if (!$self->_related_packages_can_be_synchronized($self->_initial_solution, $version)) {
			# we cannot do it, do nothing
			return sprintf __('unable to synchronize related binary packages for %s %s'),
					$package_name, $version->version_string;
		}
	}

	# update the requested package
	$package_entry->version = $version;
	if ($self->config->get_bool('cupt::resolver::track-reasons')) {
		push @{$package_entry->reasons}, $reason;
	}
	if ($self->config->get_bool('debug::resolver')) {
		my $version_string = $version->version_string;
		mydebug("install package '$package_name', version '$version_string'");
	}

	if ($o_synchronize_source_versions ne 'none') {
		$self->_synchronize_related_packages($self->_initial_solution, $version, 0, \&mydebug);
	}

	return '';
}

sub install_version ($$) {
	my ($self, $version) = @_;
	my $install_error = $self->_install_version_no_stick($version, [ 'user' ]);
	if ($install_error ne '') {
		mydie($install_error);
	}
	my $package_entry = $self->_initial_solution->get_package_entry($version->package_name);
	$package_entry->stick = 1;
	$package_entry->manually_selected = 1;
	return;
}

sub satisfy_relation_expression ($$) {
	my ($self, $relation_expression) = @_;

	# schedule checking strict relation expression, it will be checked later
	push @{$self->_strict_satisfy_relation_expressions}, $relation_expression;
	if ($self->config->get_bool('debug::resolver')) {
		my $message = "strictly satisfying relation '";
		$message .= stringify_relation_expression($relation_expression);
		$message .= "'";
		mydebug($message);
	}
	return;
}

sub unsatisfy_relation_expression ($$) {
	my ($self, $relation_expression) = @_;

	# schedule checking strict relation expression, it will be checked later
	push @{$self->_strict_unsatisfy_relation_expressions}, $relation_expression;
	if ($self->config->get_bool('debug::resolver')) {
		my $message = "strictly unsatisfying relation '";
		$message .= stringify_relation_expression($relation_expression);
		$message .= "'";
		mydebug($message);
	}
	return;
}

sub remove_package ($$) {
	my ($self, $package_name) = @_;

	my $package_entry = $self->_initial_solution->get_package_entry($package_name);
	if (not defined $package_entry) {
		$package_entry = $self->_initial_solution->set_package_entry($package_name);
	}

	if ($package_entry->stick) {
		mydie("unable to re-schedule package '%s'", $package_name);
	}
	$package_entry->version = undef;
	$package_entry->stick = 1;
	$package_entry->manually_selected = 1;
	if ($self->config->get_bool('cupt::resolver::track-reasons')) {
		push @{$package_entry->reasons}, [ 'user' ];
	}
	if ($self->config->get_bool('debug::resolver')) {
		mydebug("removing package $package_name");
	}
	return;
}

sub upgrade ($) {
	my ($self) = @_;
	foreach my $package_name ($self->_initial_solution->get_package_names()) {
		my $package = $self->cache->get_binary_package($package_name);
		my $original_version = $self->_initial_solution->get_package_entry($package_name)->version;
		# if there is original version, then at least one policy version should exist
		my $supposed_version = $self->cache->get_policy_version($package);
		defined $supposed_version or
				myinternaldie("supposed version doesn't exist");
		# no need to install the same version
		$original_version->version_string ne $supposed_version->version_string or next;
		$self->_install_version_no_stick($supposed_version, [ 'user' ]);
	}
	return;
}

sub __fair_chooser {
	my ($ref_solutions) = @_;

	# choose the solution with maximum score
	return reduce { $a->score > $b->score ? $a : $b } @$ref_solutions;
}

sub __full_chooser {
	my ($ref_solutions) = @_;
	# defer the decision until all solutions are built

	my $ref_unfinished_solution = first { ! $_->finished } @$ref_solutions;

	if (defined $ref_unfinished_solution) {
		return $ref_unfinished_solution;
	} else {
		# heh, whole solution tree has been already built?.. ok, let's choose
		# the best solution
		return __fair_chooser($ref_solutions);
	}
}

# every package version has a weight
sub _get_version_weight ($$) {
	my ($self, $version) = @_;

	return 0 if not defined $version;

	my $result = $self->cache->get_pin($version);

	$result *= 5.0 if $version->essential;

	# omitting priority 'standard' here
	if ($version->priority eq 'optional') {
		$result *= 0.9;
	} elsif ($version->priority eq 'extra') {
		$result *= 0.7;
	} elsif ($version->priority eq 'important') {
		$result *= 1.4;
	} elsif ($version->priority eq 'required') {
		$result *= 2.0;
	}

	if ($result > 0) {
		# apply the rules only to positive results so negative ones save their
		# negative effect
		my $package_name = $version->package_name;
		if ($self->cache->is_automatically_installed($package_name)) {
			$result /= 12.0;
		} elsif (not defined $self->_old_solution->get_package_entry($package_name)) {
			# new package
			$result /= 100.0;
		}
	}

	return $result;
}

sub _get_action_profit ($$$) {
	my ($self, $original_version, $supposed_version) = @_;

	my $supposed_version_weight = $self->_get_version_weight($supposed_version);
	my $original_version_weight = $self->_get_version_weight($original_version);

	if (not defined $original_version) {
		# installing the version itself gains nothing
		$supposed_version_weight -= 15;
	}

	my $result = $supposed_version_weight - $original_version_weight;
	# remove a package
	if (not defined $supposed_version) {
		$result -= 50;
		if ($result < 0) {
			$result *= 4;
		}
	}

	return $result;
}

sub __is_version_array_intersects_with_packages ($$) {
	my ($ref_versions, $solution) = @_;

	foreach my $version (@$ref_versions) {
		my $package_entry = $solution->get_package_entry($version->package_name);
		defined $package_entry or next;

		my $installed_version = $package_entry->version;
		defined $installed_version or next;

		return 1 if $version->version_string eq $installed_version->version_string;
	}
	return 0;
}

sub _is_package_can_be_removed ($$) {
	my ($self, $package_name) = @_;
	return !$self->config->get_bool('cupt::resolver::no-remove')
			|| !$self->_initial_solution->get_package_entry($package_name)->installed;
}

sub _clean_automatically_installed ($) {
	my ($self, $solution) = @_;

	# firstly, prepare all package names that can be potentially removed
	my $can_autoremove = $self->config->get_bool('cupt::resolver::auto-remove');
	my %candidates_for_remove;
	foreach my $package_name ($solution->get_package_names()) {
		$package_name ne $dummy_package_name or next;
		my $package_entry = $solution->get_package_entry($package_name);
		my $version = $package_entry->version;
		defined $version or next;
		my $original_package_entry = $self->_initial_solution->get_package_entry($package_name);
		if (defined $original_package_entry and $original_package_entry->manually_selected) {
			next;
		}
		# don't consider Essential packages for removal
		$version->essential and next;

		my $can_autoremove_this_package = $can_autoremove ?
				$self->cache->is_automatically_installed($package_name) : 0;
		my $package_was_installed = defined $self->_old_solution->get_package_entry($package_name);
		(not $package_was_installed or $can_autoremove_this_package) or next;

		if (any { $package_name =~ m/$_/ } $self->config->get_list('apt::neverautoremove')) {
			next;
		}
		# ok, candidate for removing
		$candidates_for_remove{$package_name} = 1;
	}

	my $dependency_graph = Cupt::Graph->new();
	my $main_vertex_package_name = 'main_vertex';
	do { # building dependency graph
		foreach my $package_name ($solution->get_package_names()) {
			my $version = $solution->get_package_entry($package_name)->version;
			defined $version or next;
			my @valuable_relation_expressions;
			push @valuable_relation_expressions, @{$version->pre_depends};
			push @valuable_relation_expressions, @{$version->depends};
			if ($self->config->get_bool('cupt::resolver::keep-recommends')) {
				push @valuable_relation_expressions, @{$version->recommends};
			}
			if ($self->config->get_bool('cupt::resolver::keep-suggests')) {
				push @valuable_relation_expressions, @{$version->suggests};
			}

			foreach (@valuable_relation_expressions) {
				my $satisfying_versions = $self->cache->get_satisfying_versions($_);
				foreach (@$satisfying_versions) {
					my $candidate_package_name = $_->package_name;
					exists $candidates_for_remove{$candidate_package_name} or next;
					my $candidate_version = $solution->get_package_entry($candidate_package_name)->version;
					$_->version_string eq $candidate_version->version_string or next;
					# well, this is what we need
					if (exists $candidates_for_remove{$package_name}) {
						# this is a relation between two weak packages
						$dependency_graph->add_edge($package_name, $candidate_package_name);
					} else {
						# this is a relation between installed system and a weak package
						$dependency_graph->add_edge($main_vertex_package_name, $candidate_package_name);
					}
				}
			}
		}
	};

	do { # looping the candidates
		# generally speaking, the sane way is to use Graph::TransitiveClosure,
		# but it's sloooow
		my %reachable_vertexes;
		do {
			my @current_vertexes = ($main_vertex_package_name);
			while (scalar @current_vertexes) {
				my $vertex = shift @current_vertexes;
				if (!exists $reachable_vertexes{$vertex}) {
					# ok, new vertex
					$reachable_vertexes{$vertex} = 1;
					push @current_vertexes, $dependency_graph->successors($vertex);
				}
			}
		};
		foreach my $package_name (keys %candidates_for_remove) {
			if (!exists $reachable_vertexes{$package_name}) {
				my $package_entry = $solution->set_package_entry($package_name);

				$package_entry->version = undef;
				# leave only one reason :)
				if ($self->config->get_bool('cupt::resolver::track-reasons')) {
					@{$package_entry->reasons} = ([ 'auto-remove' ]);
				}
				if ($self->config->get_bool('debug::resolver')) {
					mydebug("auto-removed package '$package_name'");
				}
			}
		}
	};

	return;
}

sub _select_solution_chooser ($) {
	my ($self) = @_;

	my $resolver_type = $self->config->get_string('cupt::resolver::type');
	my $sub_name = "__${resolver_type}_chooser";
	__PACKAGE__->can($sub_name) or
			mydie("wrong resolver type '%s'", $resolver_type);
	my $sub = \&$sub_name;
	return $sub;
}

sub _require_strict_relation_expressions ($) {
	my ($self) = @_;

	# "installing" virtual package, which will be used for strict 'satisfy' requests
	my $version = bless [] => 'Cupt::Cache::BinaryVersion';
	$version->package_name = $dummy_package_name,
	$version->source_package_name = $dummy_package_name;
	$version->version_string = '';
	$version->pre_depends = [];
	$version->depends = [];
	$version->recommends = [];
	$version->suggests = [];
	$version->breaks = [];
	$version->conflicts = [];

	my $package_entry = $self->_initial_solution->set_package_entry($dummy_package_name);
	$package_entry->version = $version;
	$package_entry->stick = 1;
	$package_entry->version->depends = $self->_strict_satisfy_relation_expressions;
	$package_entry->version->breaks = $self->_strict_unsatisfy_relation_expressions;
	$self->_initial_solution->add_version_dependencies($version);

	return;
}

# _pre_apply_action only prints debug info and changes level/score of the
# solution, not modifying packages in it, economing RAM and CPU,
# _post_apply_action will perform actual changes when the solution is picked up
# by resolver

sub _pre_apply_action ($$$$) {
	my ($self, $original_solution, $solution, $ref_action_to_apply) = @_;

	$original_solution->finished and
			myinternaldie('attempt to make changes to already finished solution');

	my $package_name_to_change = $ref_action_to_apply->{'package_name'};
	my $supposed_version = $ref_action_to_apply->{'version'};

	my $original_package_entry = $original_solution->get_package_entry($package_name_to_change);
	my $original_version = defined $original_package_entry ?
			$original_package_entry->version : undef;

	my $profit = $ref_action_to_apply->{'profit'} //
			$self->_get_action_profit($original_version, $supposed_version);

	# temporarily lower the score of the current solution to implement back-tracking
	# the bigger quality bar, the bigger chance for other solutions
	my $quality_correction = - $self->config->get_number('cupt::resolver::quality-bar') /
			($original_solution->level + 1)**0.1;

	if ($self->config->get_bool('debug::resolver')) {
		my $old_version_string = defined($original_version) ?
				$original_version->version_string : '<not installed>';
		my $new_version_string = defined($supposed_version) ?
				$supposed_version->version_string : '<not installed>';

		my $profit_string = sprintf '%.1f', $profit;
		$profit_string = "+$profit_string" if $profit > 0;
		my $quality_correction_string = sprintf '%.1f', $quality_correction;
		$quality_correction_string = "+$quality_correction_string" if $quality_correction > 0;

		my $new_solution_identifier = $solution->identifier;
		my $message = "-> ($new_solution_identifier,Δ:$profit_string,qΔ:$quality_correction_string) " .
				"trying: package '$package_name_to_change': '$old_version_string' -> '$new_version_string'";
		__mydebug_wrapper($original_solution, $message);
	}

	++$solution->level;
	$solution->score += $profit;
	$solution->score += $quality_correction;

	$solution->pending_action = $ref_action_to_apply;

	return;
}

sub _get_free_solution_identifier {
	my ($self) = @_;

	return $self->_next_free_solution_identifier++;
}

sub _pre_apply_actions_to_solution_tree {
	my ($self, $ref_solutions, $current_solution, $ref_possible_actions) = @_;

	# firstly rank all solutions
	my $position_penalty = 0;
	foreach (@$ref_possible_actions) {
		my $package_name = $_->{'package_name'};
		my $supposed_version = $_->{'version'};
		my $package_entry = $current_solution->get_package_entry($package_name);
		my $original_version = defined $package_entry ?
				$package_entry->version : undef;

		$_->{'profit'} //= $self->_get_action_profit($original_version, $supposed_version);
		$_->{'profit'} -= $position_penalty;
		++$position_penalty;
	}

	# sort them by "rank", from more good to more bad
	@$ref_possible_actions = sort { $b->{profit} <=> $a->{profit} } @$ref_possible_actions;

	# fork the solution entry and apply all the solutions by one
	foreach my $ref_action_to_apply (@$ref_possible_actions) {
		# clone the current stack to form a new one
		my $ref_cloned_solution = $current_solution->clone();

		my $new_solution_identifier = $self->_get_free_solution_identifier();
		$ref_cloned_solution->identifier = $new_solution_identifier;

		push @$ref_solutions, $ref_cloned_solution;

		# apply the solution
		$self->_pre_apply_action($current_solution, $ref_cloned_solution, $ref_action_to_apply);
	}

	# don't allow solution tree to grow unstoppably
	my $max_solution_count = $self->config->get_number('cupt::resolver::max-solution-count');
	while (scalar @$ref_solutions > $max_solution_count) {
		# find the worst solution and drop it
		my $ref_worst_solution = reduce { $a->score < $b->score ? $a : $b } @$ref_solutions;
		# temporary setting current solution to worst
		$current_solution = $ref_worst_solution;
		if ($self->config->get_bool('debug::resolver')) {
			__mydebug_wrapper($current_solution, 'dropped');
		}
		@$ref_solutions = grep { $_ ne $current_solution } @$ref_solutions;
	}
}

sub _post_apply_action {
	my ($self, $solution) = @_;

	my $ref_action_to_apply = $solution->pending_action;
	defined $ref_action_to_apply or return;

	my $package_name_to_change = $ref_action_to_apply->{'package_name'};
	my $supposed_version = $ref_action_to_apply->{'version'};

	do { # stick all requested package names
		my @additionally_requested_package_names = @{$ref_action_to_apply->{'package_names_to_stick'} // []};
		foreach my $package_name ($package_name_to_change, @additionally_requested_package_names) {
			my $package_entry = $solution->set_package_entry($package_name);
			$package_entry->stick = 1;
		}
	};

	my $package_entry_to_change = $solution->get_package_entry($package_name_to_change);
	$package_entry_to_change->version = $supposed_version;

	if (defined $ref_action_to_apply->{'fakely_satisfies'}) {
		push @{$package_entry_to_change->fake_satisfied}, $ref_action_to_apply->{'fakely_satisfies'};
	}
	if ($self->config->get_bool('cupt::resolver::track-reasons')) {
		if (defined $ref_action_to_apply->{'reason'}) {
			push @{$package_entry_to_change->reasons}, $ref_action_to_apply->{'reason'};
		}
	}
	if ($self->config->get_string('cupt::resolver::synchronize-source-versions') ne 'none') {
		# dont' do synchronization for removals
		if (defined $supposed_version) {
			$self->_synchronize_related_packages($solution, $supposed_version, 1);
		}
	}

	$solution->pending_action = undef;

	return;
}

sub __version_has_relation_expression ($$$) {
	my ($version, $dependency_group_name, $relation_expression) = @_;

	my $relation_string = stringify_relation_expression($relation_expression);
	foreach (@{$version->$dependency_group_name}) {
		if ($relation_string eq stringify_relation_expression($_)) {
			return 1;
		}
	}
	return 0;
}

sub _get_actions_to_fix_dependency ($$$$$$$) { ## no critic (ManyArgs)
	my ($self, $solution, $package_name, $ref_satisfying_versions,
			$relation_expression, $dependency_group_name) = @_;

	my @result;

	my $package_entry = $solution->get_package_entry($package_name);
	my $version = $package_entry->version;

	# install one of versions package needs
	foreach my $satisfying_version (@$ref_satisfying_versions) {
		my $satisfying_package_name = $satisfying_version->package_name;
		# can the package be updated?
		my $satisfying_package_entry = $solution->get_package_entry($satisfying_package_name);
		if (!defined $satisfying_package_entry || !$satisfying_package_entry->stick) {
			push @result, {
				'package_name' => $satisfying_package_name,
				'version' => $satisfying_version,
				'reason' => [ 'relation expression', $version, $dependency_group_name, $relation_expression ],
			};
		}
	}

	# change this package
	if (!$package_entry->stick) {
		# change version of the package
		my $other_package = $self->cache->get_binary_package($package_name);
		foreach my $other_version (@{$other_package->get_versions()}) {
			# don't try existing version
			next if $other_version->version_string eq $version->version_string;

			# let's check if other version has the same relation
			# if it has, other version will also fail so it seems there is no sense trying it
			my $found = __version_has_relation_expression($other_version,
					$dependency_group_name, $relation_expression);
			if (!$found) {
				# let's try harder to find if the other version is really appropriate for us
				foreach (@{$other_version->$dependency_group_name}) {
					# we check only relations from dependency group that caused
					# missing depends, it's not a full check, but pretty reasonable for
					# most cases; in rare cases that some problematic dependency
					# migrated to other dependency group, it will be revealed at
					# next check run

					# fail revealed that no one of available versions of dependent
					# packages can satisfy the main package, so if some relation's
					# satisfying versions are subset of failed ones, the version won't
					# be accepted as a resolution
					my $has_resolution_outside = 0;
					my $ref_candidate_satisfying_versions = $self->cache->get_satisfying_versions($_);
					foreach (@$ref_candidate_satisfying_versions) {
						my $candidate_package_name = $_->package_name;
						my $candidate_version_string = $_->version_string;
						my $is_candidate_appropriate = 1;
						foreach (@$ref_satisfying_versions) {
							next if $_->package_name ne $candidate_package_name;
							next if $_->version_string ne $candidate_version_string;
							# this candidate has fallen into dead-end
							$is_candidate_appropriate = 0;
							last;
						}
						if ($is_candidate_appropriate) {
							# more wide relation, can't say nothing bad with it at time being
							$has_resolution_outside = 1;
							last;
						}
					}
					$found = !$has_resolution_outside;
					last if $found;
				}
				if (!$found) {
					# other version seems to be ok
					push @result, {
						'package_name' => $package_name,
						'version' => $other_version,
						'reason' => [ 'relation expression', $version, $dependency_group_name, $relation_expression ],
					};
				}
			}
		}

		if ($self->_is_package_can_be_removed($package_name)) {
			# remove the package
			push @result, {
				'package_name' => $package_name,
				'version' => undef,
				'reason' => [ 'relation expression', $version, $dependency_group_name, $relation_expression ],
			};
		}
	}

	return @result;
}

sub __prepare_stick_requests ($) {
	my ($ref_possible_actions) = @_;

	# the each next action receives one more additional stick request to not
	# interfere with all previous solutions
	my @package_names;
	foreach my $ref_action (@$ref_possible_actions) {
		$ref_action->{'package_names_to_stick'} = [ @package_names ];
		push @package_names, $ref_action->{'package_name'};
	}
	return;
}

sub _propose_solution {
	my ($self, $solution, $sub_callback) = @_;

	# build "user-frienly" version of solution
	my %suggested_packages;
	foreach my $package_name ($solution->get_package_names()) {
		next if $package_name eq $dummy_package_name;
		my $other_package_entry = $solution->get_package_entry($package_name);
		$suggested_packages{$package_name}->{'version'} = $other_package_entry->version;
		$suggested_packages{$package_name}->{'reasons'} = $other_package_entry->reasons;
		my $original_package_entry = $self->_initial_solution->get_package_entry($package_name);
		$suggested_packages{$package_name}->{'manually_selected'} =
				(defined $original_package_entry and $original_package_entry->manually_selected);
	}

	# suggest found solution
	if ($self->config->get_bool('debug::resolver')) {
		__mydebug_wrapper($solution, 'proposing this solution');
	}
	my $user_answer = $sub_callback->(\%suggested_packages);
	if ($self->config->get_bool('debug::resolver') and defined $user_answer) {
		if ($user_answer) {
			__mydebug_wrapper($solution, $user_answer ? 'accepted' : 'declined');
		}
	}

	return $user_answer;
}

sub _filter_unsynchronizeable_actions {
	my ($self, $solution, $ref_actions) = @_;

	my @new_possible_actions;
	foreach my $possible_action (@$ref_actions) {
		my $version = $possible_action->{'version'};
		if (not defined $version or
			$self->_related_packages_can_be_synchronized($solution, $version))
		{
			push @new_possible_actions, $possible_action;
		} else {
			# we cannot proceed with it, so try deleting related packages
			my @unsynchronizeable_package_names = $self->_get_unsynchronizeable_related_package_names(
					$solution, $version);
			foreach my $unsynchronizeable_package_name (@unsynchronizeable_package_names) {
				next if $solution->get_package_entry($unsynchronizeable_package_name)->stick;

				if (none {
						$_->{'package_name'} eq $unsynchronizeable_package_name and
						not defined $_->{'version'}
					} @new_possible_actions)
				{
					unshift @new_possible_actions, {
						'package_name' => $unsynchronizeable_package_name,
						'version' => undef,
						'reason' => [ 'sync', $version->package_name ],
					};
				}
			}
			if ($self->config->get_bool('debug::resolver')) {
				__mydebug_wrapper($solution, sprintf(
						'cannot consider installing %s %s: unable to synchronize related packages (%s)',
						$version->package_name, $version->version_string,
						join(', ', @unsynchronizeable_package_names)));
			}
		}
	}
	return @new_possible_actions;
}

sub _is_soft_dependency_ignored {
	my ($self, $version, $dependency_group_name, $relation_expression, $ref_satisfying_versions) = @_;

	my $was_satisfied_in_the_past = __is_version_array_intersects_with_packages(
				$ref_satisfying_versions, $self->_old_solution);

	return 0 if $was_satisfied_in_the_past;

	if (not $self->config->get_bool("apt::install-$dependency_group_name")) {
		return 1;
	}

	my $old_package_entry = $self->_old_solution->get_package_entry($version->package_name);
	if (defined $old_package_entry) {
		my $old_version = $old_package_entry->version;
		if (defined $old_version and __version_has_relation_expression($old_version,
			$dependency_group_name, $relation_expression))
		{
			# the fact that we are here means that the old version of this package
			# had exactly the same relation expression, and it was unsatisfied
			# so, upgrading the version doesn't bring anything new
			return 1;
		}
	}

	return 0;
}

sub resolve ($$) { ## no critic (RequireFinalReturn)
	my ($self, $sub_accept) = @_;

	my $sub_solution_chooser = $self->_select_solution_chooser();
	if ($self->config->get_bool('debug::resolver')) {
		mydebug('started resolving');
	}
	$self->_require_strict_relation_expressions();

	my @dependency_groups = @{$self->_get_dependency_groups()};

	my @solutions = ($self->_initial_solution->clone());
	$solutions[0]->identifier = 0;
	$solutions[0]->prepare();

	my $current_solution;

	# for each package entry 'count' will contain the number of failures
	# during processing these package
	# { package_name => count }...
	my %failed_counts;

	my $check_failed;

	my $cache = $self->cache;

	while (1) {
		# continue only if we have at least one solution pending, otherwise we have a great fail
		scalar @solutions or return 0;

		my @possible_actions;

		# choosing the solution to process
		$current_solution = $sub_solution_chooser->(\@solutions);
		if (defined $current_solution->pending_action) {
			$current_solution->prepare();
			$self->_post_apply_action($current_solution);
		}

		# for the speed reasons, we will correct one-solution problems directly in MAIN_LOOP
		# so, when an intermediate problem was solved, maybe it breaks packages
		# we have checked earlier in the loop, so we schedule a recheck
		#
		# once two or more solutions are available, loop will be ended immediately
		my $recheck_needed = 1;
		MAIN_LOOP:
		while ($recheck_needed) {
			my $package_entry;

			$recheck_needed = 0;

			# clearing check_failed
			$check_failed = 0;

			# to speed up the complex decision steps, if solution stack is not
			# empty, firstly check the packages that had a problem
			my @packages_in_order = sort {
				($failed_counts{$b} // 0) <=> ($failed_counts{$a} // 0) or
				$a cmp $b
			} $current_solution->get_package_names();

			foreach my $ref_dependency_group (@dependency_groups) {
				my $dependency_group_name = $ref_dependency_group->{'name'};
				my $dependency_group_target = $ref_dependency_group->{'target'};
				my $dependency_group_index = $ref_dependency_group->{'index'};

				PACKAGE:
				foreach my $package_name (@packages_in_order) {
					$package_entry = $current_solution->get_package_entry($package_name);

					# skip check if already marked as checked
					vec($package_entry->checked_bits, $dependency_group_index, 1) and next;

					my $version = $package_entry->version;
					defined $version or next;

					foreach my $relation_expression (@{$version->$dependency_group_name}) {
						my $ref_satisfying_versions = $cache->get_satisfying_versions($relation_expression);
						my $intersects = __is_version_array_intersects_with_packages(
								$ref_satisfying_versions, $current_solution);
						if ($dependency_group_target eq 'normal') {
							# check if relation is already satisfied
							if (not $intersects) {
								if ($dependency_group_name eq 'recommends' or $dependency_group_name eq 'suggests') {
									# this is a soft dependency

									if (any { $_ == $relation_expression } @{$package_entry->fake_satisfied}) {
										# this soft relation expression was already fakely satisfied (score penalty)
										next;
									}

									if ($self->_is_soft_dependency_ignored($version, $dependency_group_name,
											$relation_expression, $ref_satisfying_versions))
									{
										next;
									}

									# ok, then we have one more possible solution - do nothing at all
									push @possible_actions, {
										'package_name' => $package_name,
										'version' => $version,
										# set profit manually, as we are inserting fake action here
										'profit' => ($dependency_group_name eq 'recommends' ? -200 : -50),
										'fakely_satisfies' => $relation_expression,
										'reason' => undef,
									};
								}

								# mark package as failed one more time
								++$failed_counts{$package_name};

								# for resolving we can do:
								push @possible_actions, $self->_get_actions_to_fix_dependency(
										$current_solution, $package_name, $ref_satisfying_versions,
										$relation_expression, $dependency_group_name);

								if ($self->config->get_bool('debug::resolver')) {
									my $stringified_relation = stringify_relation_expression($relation_expression);
									__mydebug_wrapper($current_solution, "problem: package '$package_name': " .
											"unsatisfied $dependency_group_name '$stringified_relation'");
								}
								$check_failed = 1;

								if (scalar @possible_actions == 1) {
									$self->_pre_apply_action($current_solution, $current_solution, $possible_actions[0]);
									$self->_post_apply_action($current_solution);
									@possible_actions = ();
									$recheck_needed = 1;
									redo PACKAGE;
								}
								$recheck_needed = 0;
								last MAIN_LOOP;
							}
						} else {
							# check if relation is accidentally satisfied
							if ($intersects) {
								# so, this can conflict... check it deeper on the fly
								my $conflict_found = 0;
								foreach my $satisfying_version (@$ref_satisfying_versions) {
									my $other_package_name = $satisfying_version->package_name;

									# package can't conflict (or break) with itself
									$other_package_name ne $package_name or next;

									my $other_package_entry = $current_solution->get_package_entry($other_package_name);

									# is the package installed?
									defined $other_package_entry or next;

									# does the package have an installed version?
									defined($other_package_entry->version) or next;

									# is this our version?
									$other_package_entry->version->version_string eq $satisfying_version->version_string or next;

									# :(
									$conflict_found = 1;

									# additionally, in case of absense of stick, also contribute to possible actions
									if (!$other_package_entry->stick) {
										# so change it
										my $other_package = $cache->get_binary_package($other_package_name);
										foreach my $other_version (@{$other_package->get_versions()}) {
											# don't try existing version
											next if $other_version->version_string eq $satisfying_version->version_string;

											push @possible_actions, {
												'package_name' => $other_package_name,
												'version' => $other_version,
												'reason' => [ 'relation expression', $version, $dependency_group_name, $relation_expression ],
											};
										}

										if ($self->_is_package_can_be_removed($other_package_name)) {
											# or remove it
											push @possible_actions, {
												'package_name' => $other_package_name,
												'version' => undef,
												'reason' => [ 'relation expression', $version, $dependency_group_name, $relation_expression ],
											};
										}
									}
								}

								if ($conflict_found) {
									$check_failed = 1;

									# mark package as failed one more time
									++$failed_counts{$package_name};

									if (!$package_entry->stick) {
										# change version of the package
										my $package = $cache->get_binary_package($package_name);
										foreach my $other_version (@{$package->get_versions()}) {
											# don't try existing version
											next if $other_version->version_string eq $version->version_string;

											push @possible_actions, {
												'package_name' => $package_name,
												'version' => $other_version,
												'reason' => [ 'relation expression', $version, $dependency_group_name, $relation_expression ],
											};
										}

										if ($self->_is_package_can_be_removed($package_name)) {
											# remove the package
											push @possible_actions, {
												'package_name' => $package_name,
												'version' => undef,
												'reason' => [ 'relation expression', $version, $dependency_group_name, $relation_expression ],
											};
										}
									}

									if ($self->config->get_bool('debug::resolver')) {
										my $stringified_relation = stringify_relation_expression($relation_expression);
										__mydebug_wrapper($current_solution, "problem: package '$package_name': " .
												"satisfied $dependency_group_name '$stringified_relation'");
									}
									$recheck_needed = 0;
									last MAIN_LOOP;
								}
							}
						}
					}
					$current_solution->validate($package_name, $dependency_group_index);
				}
			}
		}

		if (!$check_failed) {
			# if the solution was only just finished
			if (not $current_solution->finished) {
				if ($self->config->get_bool('debug::resolver')) {
					__mydebug_wrapper($current_solution, 'finished');
				}

				# clean up automatically installed by resolver and now unneeded packages
				$self->_clean_automatically_installed($current_solution);

				$current_solution->finished = 1;

				# now, as we use partial checks (using set_package_entry/validate), before
				# we present a solution it's a good idea to validate it from scratch finally:
				# if it ever turns that partial checks pass a wrong solution, we must
				# catch it
				#
				# so, we schedule a last check round for a solution, but as it already has
				# 'finished' property set, if the problem will appear, _pre_apply_action will
				# die loudly
				foreach my $package_name ($current_solution->get_package_names()) {
					# invalidating all solution
					$current_solution->set_package_entry($package_name);
				}
				next;
			}

			# resolver can refuse the solution
			my $ref_new_selected_solution = $sub_solution_chooser->(\@solutions);

			if ($ref_new_selected_solution ne $current_solution) {
				# ok, process other solution
				next;
			}

			my $user_answer = $self->_propose_solution($current_solution, $sub_accept);
			if (!defined $user_answer) {
				# user has selected abandoning all further efforts
				return undef;
			} elsif ($user_answer) {
				# yeah, this is end of our tortures
				return 1;
			} else {
				# caller hasn't accepted this solution, well, go next...

				# purge current solution
				@solutions = grep { $_ ne $current_solution } @solutions;
			}
		} else {
			if ($self->config->get_string('cupt::resolver::synchronize-source-versions') eq 'hard') {
				# if we have to synchronize source versions, can related packages be updated too?
				# filter out actions that don't match this criteria
				@possible_actions = $self->_filter_unsynchronizeable_actions(
						$current_solution, \@possible_actions);
			}

			__prepare_stick_requests(\@possible_actions);

			# purge current solution
			@solutions = grep { $_ ne $current_solution } @solutions;

			if (scalar @possible_actions) {
				$self->_pre_apply_actions_to_solution_tree(\@solutions, $current_solution, \@possible_actions);
			} else {
				if ($self->config->get_bool('debug::resolver')) {
					__mydebug_wrapper($current_solution, 'no solutions');
				}
			}
		}
	}
}

1;

