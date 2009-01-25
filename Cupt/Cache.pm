package Cupt::Cache;
# TODO: implement checking for signedness

use 5.10.0;
use strict;
use warnings;

use Cupt::Core;
use Cupt::Cache::Pkg;
use Cupt::Cache::BinaryVersion;
use Cupt::Cache::SourceVersion;
use Cupt::System::State;

=head1 FIELDS

=head2 can_provide

{ I<virtual_package> => [ I<package_name>... ] }

For each I<virtual_package> this field contains the list of I<package_name>s
that B<can> provide given I<virtual_package>. Depending of package versions,
some versions of the some of <package_name>s may provide and may not provide
given I<virtual_package>. This field exists solely for
I<get_satisfying_versions> subroutine for rapid lookup.

=cut

use fields qw(source_packages binary_packages config pin_settings system_state can_provide);

sub new {
	my $class = shift;
	my $self = fields::new($class);

	$self->{config} = shift;
	$self->{pin_settings} = [];

	my $ref_index_entries;
	eval {
		$ref_index_entries = $self->_parse_sources_lists();
	};
	if (mycatch()) {
		myerr("error while parsing sources list");
		myredie();
	}

	# determining which parts of cache we wish to build
	my %build_config = (
		'-source' => 1,
		'-binary' => 1,
		'-installed' => 1,
		@_ # applying passed parameters
	);

	if ($build_config{'-installed'}) {
		# read system settings
		$self->{system_state} = new Cupt::System::State($self->{config}, $self);
	}

	foreach my $ref_index_entry (@$ref_index_entries) {
		my $index_file_to_parse = $self->_path_of_source_list($ref_index_entry);
		my $source_type = $ref_index_entry->{'type'};
		# don't parse unneeded indexes
		if (($source_type eq 'deb' && $build_config{'-binary'}) ||
			($source_type eq 'deb-src' && $build_config{'-source'}))
		{
			eval {
				my $base_uri = $ref_index_entry->{'uri'};
				my $ref_release_info = __get_release_info($self->_path_of_release_list($ref_index_entry));
				$ref_release_info->{component} = $ref_index_entry->{'component'};
				$self->_process_index_file($index_file_to_parse, \$base_uri, $source_type, $ref_release_info);
			};
			if (mycatch()) {
				mywarn("skipped index file '%s'", $index_file_to_parse);
			}
		}
	}

	# reading pin settings
	my $pin_settings_file = $self->_path_of_preferences();
	$self->_parse_preferences($pin_settings_file) if -r $pin_settings_file;

	return $self;
}

sub get_pin {
	my ($self, $version) = @_;
	my $result;

	my $update_pin = sub ($) {
		if (!defined($result)) {
			$result = $_[0];
		} elsif ($result < $_[0]) {
			$result = $_[0];
		}
	};

	# 'available as' array, excluding local version if it present
	my @avail_as = @{$version->{avail_as}};

	# look for installed package?
	if ($version->is_local()) {
		# yes, this version is installed
		$update_pin->(100);
		shift @avail_as;
	}

	# release-dependent settings
	my $default_release = $self->{config}->var("apt::default-release");
	foreach (@avail_as) {
		if (defined($default_release)) {
			if ($_->{release}->{archive} eq $default_release ||
				$_->{release}->{codename} eq $default_release)
			{
				$update_pin->(990);
				last; # no sense to search further, this is maximum
			}
		}
		if ($_->{release}->{archive} eq 'experimental') {
			$update_pin->(1);
		} else {
			$update_pin->(500);
		}
	}

	# looking in pin settings
	PIN:
	foreach my $pin (@{$self->{pin_settings}}) {
		if (exists $pin->{'package_name'}) {
			my $value = $pin->{'package_name'};
			$version->{package_name} =~ m/$value/ or next PIN;
		}
		if (exists $pin->{'source_name'}) {
			my $value = $pin->{'source_name'};
			$version->{source_name} =~ m/$value/ or next PIN;
		}
		if (exists $pin->{'version'}) {
			my $value = $pin->{'version'};
			$version->{version} =~ m/$value/ or next PIN;
		}
		if (exists $pin->{'base_uri'}) {
			my $value = $pin->{'base_uri'};

			my $found = 0;
			foreach (@avail_as) {
				if ($_->{base_uri} =~ m/$value/) {
					$found = 1;
					last;
				}
			}
			$found or next PIN;
		}
		if (exists $pin->{'release'}) {
			while (my ($key, $value) = each %{$pin->{'release'}}) {
				my $value = $value;

				my $found = 0;
				foreach (@avail_as) {
					if (defined $_->{release}->{$key} &&
						$_->{release}->{$key} =~ m/$value/)
					{
						$found = 1;
						last;
					}
				}
				$found or next PIN;
			}
		}

		# yeah, all conditions satisfied here
		$update_pin->($pin->{'value'});
	}

	# discourage downgrading for pins <= 1000
	# downgradings will have pin <= 0
	if ($result <= 1000) {
		my $package_name = $version->{package_name};
		my $installed_version_string = $self->{system_state}->get_installed_version_string($package_name);
		if (defined($installed_version_string)
			&& Cupt::Core::compare_version_strings($installed_version_string, $version->{version}) > 0)
		{
			$result -= 1000;
		}
	}

	return $result;
}

sub get_binary_package {
	my ($self, $package_name) = @_;
	if (exists $self->{binary_packages}->{$package_name}) {
		return $self->{binary_packages}->{$package_name};
	} else {
		return undef;
	}
};

# return array reference of sorted versions by "candidatness" in descending order
sub get_sorted_pinned_versions {
	my ($self, $package) = @_;

	my @result;
	foreach my $version (@{$package->versions()}) {
		push @result, { 'version' => $version, 'pin' => $self->get_pin($version) };
	}

	do {
		use sort 'stable';
		# sort in descending order, first key is pin, second is version string
		@result = sort {
			$b->{'pin'} <=> $a->{'pin'} or 
			compare_versions($b->{'version'}, $a->{'version'})
		} @result;
	};

	return \@result;
}

sub get_policy_version {
	my ($self, $package) = @_;

	# selecting by policy
	# we assume that every existent package have at least one version
	# this is how we add versions in 'Cupt::Cache::&_process_index_file'

	# so, just return version with maximum "candidatness"
	return $self->get_sorted_pinned_versions($package)->[0]->{'version'};
}

sub _get_satisfying_versions_for_one_relation {
	my ($self, $relation) = @_;
	my $package_name = $relation->{package_name};

	# caching results
	state %cache;
	my $key = join(",",
			$self,
			$package_name,
			defined($relation->{relation}) ? $relation->{relation} : "",
			defined($relation->{version}) ? $relation->{version} : ""
	);
	if (exists $cache{$key}) {
		return @{$cache{$key}};
	}

	my $package = $self->get_binary_package($package_name);
	my @result;

	if (defined($package)) {
		# if such binary package exists
		my $ref_sorted_versions = $self->get_sorted_pinned_versions($package);
		foreach (@$ref_sorted_versions) {
			my $version = $_->{'version'};
			push @result, $version if $relation->satisfied_by($version->{version});
		}
	}

	# virtual package can only be considered if no relation sign is specified
	if (!defined($relation->{relation}) && exists $self->{can_provide}->{$package_name}) {
		# looking for reverse-provides
		foreach (@{$self->{can_provide}->{$package_name}}) {
			my $reverse_provide_package = $self->get_binary_package($_);
			defined ($reverse_provide_package) or next;
			foreach (@{$self->get_sorted_pinned_versions($reverse_provide_package)}) {
				my $version = $_->{version};
				defined $version->{provides} or next;
				foreach (@{$version->{provides}}) {
					my $provides_package_name = $_;
					if ($provides_package_name eq $package_name) {
						# ok, this particular version does provide this virtual package
						push @result, $version;
					}
				}
			}
		}
	}

	$cache{$key} = \@result;
	return @result;
}

=head2 get_satisfying_versions

returns reference to array of versions that satisfy relation, if no any version can satisfy the relation, return empty array

Parameters - (C<self>, C<relation_expression>)

C<self> - object
C<relation_expression> - reference to Cupt::Cache::Relation, or relation OR
group (see documentation for Cupt::Cache::Relation for the info about OR
groups)

=cut

sub get_satisfying_versions ($$) {
	my ($self, $relation_expression) = @_;

	if (UNIVERSAL::isa($relation_expression, 'Cupt::Cache::Relation')) {
		# relation expression is just one relation
		return [ $self->_get_satisfying_versions_for_one_relation($relation_expression) ];
	} else {
		# othersise it's OR group of expressions
		return [ map { $self->_get_satisfying_versions_for_one_relation($_) } @$relation_expression ];
	}
}

our %_empty_release_info = (
	'version' => undef,
	'description' => undef,
	'signed' => 0,
	'vendor' => undef,
	'label' => undef,
	'archive' => undef,
	'codename' => undef,
	'date' => undef,
	'valid-until' => undef,
	'architectures' => undef,
);

sub __get_release_info {
	my $file = shift;

	my %release_info = %_empty_release_info;

	open(RELEASE, '<', $file) or mydie("unable to open release file '%s'", $file);
	my $field_name = undef;
	eval {
		while (<RELEASE>) {
			(($field_name, my $field_value) = ($_ =~ m/^((?:\w|-)+?): (.*)/)) # '$' implied in regexp
				or last;

			given ($field_name) {
				when ('Origin') { $release_info{vendor} = $field_value }
				when ('Label') { $release_info{label} = $field_value }
				when ('Suite') { $release_info{archive} = $field_value }
				when ('Codename') { $release_info{codename} = $field_value }
				when ('Date') { $release_info{date} = $field_value }
				when ('Valid-Until') { $release_info{valid_until} = $field_value }
				when ('Architectures') { $release_info{architectures} = [ split / /, $field_value ] }
				when ('Description') {
					$release_info{description} = $field_value;
					if ($field_value =~ m/([0-9a-z._-]+)/) {
						$release_info{version} = $1;
					}
				}
			}

			undef $field_name;
		}
	};
	if (mycatch()) {
		myerr("error parsing release file '%s', line '%d'", $file, $.);
		myredie();
	}
	if (!defined($release_info{description})) {
		mydie("no description specified in release file '%s'", $file);
	}
	if (!defined($release_info{vendor})) {
		mydie("no vendor specified in release file '%s'", $file);
	}
	if (!defined($release_info{archive})) {
		mydie("no archive specified in release file '%s'", $file);
	}
	if (!defined($release_info{codename})) {
		mydie("no codename specified in release file '%s'", $file);
	}

	close(RELEASE) or mydie("unable to close release file '%s'", $file);
	return \%release_info;
}

sub _parse_sources_lists {
	my $self = shift;
	my $root_prefix = $self->{config}->var('dir');
	my $etc_dir = $self->{config}->var('dir::etc');

	my $parts_dir = $self->{config}->var('dir::etc::sourceparts');
	my @source_files = glob("$root_prefix$etc_dir/$parts_dir/*");

	my $main_file = $self->{config}->var('dir::etc::sourcelist');
	push @source_files, "$root_prefix$etc_dir/$main_file";

	my @result;
	foreach (@source_files) {
		push @result, __parse_source_list($_);
	}

	return \@result;
}

sub __parse_source_list {
	my $file = shift;
	my @result;
	open(HFILE, '<', "$file") or mydie("unable to open file %s: %s", $file, $!);
	while (<HFILE>) {
		chomp;
		# skip all empty lines and lines with comments
		next if m/^\s*(?:#.*)?$/;

		my %entry;
		($entry{'type'}, $entry{'uri'}, $entry{'distribution'}, my @sections) = split / +/;
		#print %entry;

		mydie("incorrent source line at file %s, line %d", $file, $.) if (!scalar @sections);
		mydie("incorrent source type at file %s, line %d", $file, $.)
			if ($entry{'type'} ne 'deb' && $entry{'type'} ne 'deb-src');

		map { $entry{'component'} = $_; push @result, { %entry }; } @sections;
	}
	close(HFILE) or mydie("unable to close file %s: %s", $file, $!);
	return @result;
}

sub _parse_preferences {
	my ($self, $file) = @_;

	# we are parsing triades like:

	# Package: perl
	# Pin: o=debian,a=unstable
	# Pin-Priority: 800

	# Source: unetbootin
	# Pin: a=experimental
	# Pin-Priority: 1100

	sub glob_to_regex ($) {
		$_[0] =~ s/\*/.*?/g;
		$_[0] =~ s/^/.*?/g;
		$_[0] =~ s/$/.*/g;
	}

	open(PREF, '<', $file) or mydie("unable to open file %s: %s'", $file, $!);
	while (<PREF>) {
		chomp;
		# skip all empty lines and lines with comments
		next if m/^\s*(?:#.*)?$/;

		# ok, real triade should be here
		my %pin_result;

		do { # processing first line
			m/^(Package|Source): (.*)/ or
					mydie("bad package/source line at file '%s', line '%u'", $file, $.);

			my $name_type = ($1 eq 'Package' ? 'package_name' : 'source_name');
			my $name_value = $2;
			glob_to_regex($name_value);

			$pin_result{$name_type} = $name_value;
		};

		do { # processing second line
			my $pin_line = <PREF>;
			defined($pin_line) or
					mydie("no pin line at file '%s' line '%u'", $file, $.);

			$pin_line =~ m/^Pin: (\w+?) (.*)/ or
					mydie("bad pin line at file '%s' line '%u'", $file, $.);

			my $pin_type = $1;
			my $pin_expression = $2;
			given ($pin_type) {
				when ('release') {
					my @conditions = split /,/, $pin_expression;
					scalar @conditions or
							mydie("bad release expression at file '%s' line '%u'", $file, $.);

					foreach (@conditions) {
						m/^(\w)=(.*)/ or
								mydie("bad condition in release expression at file '%s' line '%u'", $file, $.);

						my $condition_type = $1;
						my $condition_value = $2;
						given ($condition_type) {
							when ('a') { $pin_result{'release'}->{'archive'} = $condition_value; }
							when ('v') { $pin_result{'release'}->{'version'} = $condition_value; }
							when ('c') { $pin_result{'release'}->{'component'} = $condition_value; }
							when ('n') { $pin_result{'release'}->{'codename'} = $condition_value; }
							when ('o') { $pin_result{'release'}->{'vendor'} = $condition_value; }
							when ('l') { $pin_result{'release'}->{'label'} = $condition_value; }
							default {
								mydie("bad condition type (should be one of 'a', 'v', 'c', 'n', 'o', 'l') " . 
										"in release expression at file '%s' line '%u'", $file, $.);
							}
						}
					}
				}
				when ('version') {
					glob_to_regex($pin_expression);
					$pin_result{'version'} = $pin_expression;
				}
				when ('origin') { # this is 'base_uri', really...
					$pin_result{'base_uri'} = $pin_expression;
				}
				default {
					mydie("bad pin type (should be one of 'release', 'version', 'origin') " . 
							"at file '%s' line '%u'", $file, $.);
				}
			}
		};

		do { # processing third line
			my $priority_line = <PREF>;
			defined($priority_line) or
					mydie("no priority line at file '%s' line '%u'", $file, $.);

			$priority_line =~ m/^Pin-Priority: ([+-]?\d+)/ or
					mydie("bad priority line at file '%s' line '%u'", $file, $.);

			my $priority = $1;
			$pin_result{'value'} = $priority;
		};

		# adding to storage
		push @{$self->{'pin_settings'}}, \%pin_result;
	}

	close(PREF) or mydie("unable to close file %s: %s", $file, $!);
}

sub _process_provides_in_index_file {
	my ($self, $file) = @_;

	open(ENTRIES, "/usr/bin/grep-dctrl -r -n -F Provides '.' -s Package,Provides $file |") or
			mydie("unable to open grep-dctrl pipe on file '%s': %s", $file, $!);

	# entries will be in format:
	#
	# <package_name>
	# <provides list>
	# <newline>
	# ...

	eval {
		while(<ENTRIES>) {
			chomp;
			my $package_name = $_;

			$_ = readline(ENTRIES);
			chomp;
			my @provides = split /\s*,\s*/, $_;

			foreach (@provides) {
				push @{$self->{can_provide}->{$_}}, $package_name;
			}
			readline(ENTRIES) eq "\n" or
					mydie("expected newline, but haven't got it");
		}
	};
	if (mycatch()) {
		myerr("error parsing provides in index file '%s'", $file);
		myredie();
	}

	close(ENTRIES) or $! == 0 or # '$! == 0' - no entries found, nothing bad
			mydie("unable to close grep-dctrl pipe on file '%s'", $file);
}

sub _process_index_file {
	my ($self, $file, $ref_base_uri, $type, $ref_release_info) = @_;

	my $version_class;
	my $packages_storage;
	if ($type eq 'deb') {
		$version_class = 'Cupt::Cache::BinaryVersion';
		$packages_storage = \$self->{binary_packages};
	} elsif ($type eq 'deb-src') {
		$version_class = 'Cupt::Cache::SourceVersion';
		$packages_storage = \$self->{source_packages};
		mywarn("not parsing deb-src index '%s' (parsing code is broken now)", $file);
		return;
	}

	my $fh;
	open($fh, '<', $file) or mydie("unable to open index file '%s'", $file);
	open(OFFSETS, "/bin/grep -b '^Package: ' $file |"); 

	eval {
		while (<OFFSETS>) {
			if (m/^(\d+):Package: (.*)/) { # '$' implied in regexp
				my $package_name = $2;

				# offset is returned by grep -b, and we skips 'Package: <...>' line additionally
				my $offset = $1 + length("Package: $package_name\n");

				# check it for correctness
				($package_name =~ m/^$package_name_regex$/)
					or mydie("bad package name '%s'", $package_name);

				# end of entry, so creating new package
				$$packages_storage->{$package_name} //= Cupt::Cache::Pkg->new();

				Cupt::Cache::Pkg::add_entry($$packages_storage->{$package_name}, $version_class,
						$package_name, $fh, $offset, $ref_base_uri, $ref_release_info);
			} else {
				mydie("expected 'Package' line, but haven't got it");
			}
		}
	};
	if (mycatch()) {
		myerr("error parsing index file '%s'", $file);
		myredie();
	}

	close(OFFSETS) or mydie("unable to close grep pipe");

	# additionally, preparse Provides fields...
	$self->_process_provides_in_index_file($file);
}

sub _path_of_base_uri {
	my $self = shift;
	my $entry = shift;

	# "http://ftp.ua.debian.org" -> "ftp.ua.debian.org"
	(my $uri_prefix = $entry->{'uri'}) =~ s[^\w+://][];

	# "ftp.ua.debian.org/debian" -> "ftp.ua.debian.org_debian"
	$uri_prefix =~ tr[/][_];

	my $dirname = join('',
		$self->{config}->var('dir'),
		$self->{config}->var('dir::state'),
		'/',
		$self->{config}->var('dir::state::lists')
	);

	my $base_uri_part = join('_',
		$uri_prefix,
		'dists',
		$entry->{'distribution'}
	);

	return join('', $dirname, '/', $base_uri_part);
}

sub _path_of_source_list {
	my $self = shift;
	my $entry = shift;

	my $arch = $self->{config}->var('apt::architecture');
	my $suffix = ($entry->{'type'} eq 'deb') ? "binary-${arch}_Packages" : 'source_Sources';

	my $filename = join('_', $self->_path_of_base_uri($entry), $entry->{'component'}, $suffix);

	return $filename;
}

sub _path_of_release_list {
	my $self = shift;
	my $entry = shift;

	my $filename = join('_', $self->_path_of_base_uri($entry), 'Release');

	return $filename;
}

sub _path_of_preferences {
	my ($self) = @_;

	my $root_prefix = $self->{config}->var('dir');
	my $etc_dir = $self->{config}->var('dir::etc');

	my $leaf = $self->{config}->var('dir::etc::preferences');

	return "$root_prefix$etc_dir/$leaf";
}

1;

