#!/usr/bin/env perl
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use Test::More qw/no_plan/;

my $node = PostgreSQL::Test::Cluster->new('primary');

$node->init();
$node->start();

$node->safe_psql('postgres','CREATE EXTENSION IF NOT EXISTS pg_kaboom');

is ($node->safe_psql('postgres',"select extname from pg_extension where extname = 'pg_kaboom'"),
	'pg_kaboom',
	'successfully created the extension'
);
