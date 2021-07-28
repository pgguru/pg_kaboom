# pg_kaboom

> Where's the kaboom?! There's supposed to be an Earth-shattering kaboom!

This extension serves to crash postgresql in multiple varied and destructive ways.

## But why?

Testing of failover can be hard to do from SQL; some things are nice to expose via SQL functions.  This is one of those things.

## Is this safe?

**Hell, no**.  Under no circumstances should you use this extension on a production cluster; this is purely for testing things out in a development environment.

We require you to set a GUC variable `pg_kaboom.disclaimer` to a magic value in order for any of these functions to do anything.  That said, there are often times where simulating different breakage scenarios are useful.  Under no way are we liable for anything you do with this software.  This is provided without warranty and complete disclaimer.

<blink>This is your final warning!  You *will* lose data!</blink>

## Installation

```console
$ git clone git@github.com:pgguru/pg_kaboom.git
$ cd pg_kaboom
$ make PG_CONFIG=path/to/pg_config && make install PG_CONFIG=path/to/pg_config
$ psql -c 'CREATE EXTENSION pg_kaboom'
```

