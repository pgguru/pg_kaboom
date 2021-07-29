#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/builtins.h"
#include "storage/ipc.h"

#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

#define PG_KABOOM_DISCLAIMER "I can afford to lose this data and server"

static char *disclaimer;
static char *pgdata_path = NULL;
static bool execute = false;

static void validate_we_can_blow_up_things();
static void validate_we_can_restart();
static void restart_database();
static void load_pgdata_path();
static void fill_disk_at_path(char *path, char *subpath);
static void command_with_path(char *command, char *path, bool detach);
static void command_with_path_internal(char *command, char *arg1, char *arg2, bool detach);

PG_MODULE_MAGIC;

void _PG_init(void);
void _PG_fini(void);

Datum pg_kaboom(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_kaboom);

void _PG_init(void)
{
	/* ... C code here at time of extension loading ... */
	DefineCustomStringVariable("pg_kaboom.disclaimer",
							   gettext_noop("Disclaimer variable you must set for the pg_kaboom extension to work. Required value is: '"
											PG_KABOOM_DISCLAIMER "'"),
							   NULL,
							   &disclaimer,
							   "",
							   PGC_USERSET, 0,
							   NULL, NULL, NULL);

	DefineCustomBoolVariable("pg_kaboom.execute",
							   gettext_noop("Whether to actually run the commands that are generated"),
							   NULL,
							   &execute,
							   0,
							   PGC_USERSET, 0,
							   NULL, NULL, NULL);

	load_pgdata_path();
}

void _PG_fini(void)
{
	/* ... C code here at time of extension unloading ... */
}

Datum pg_kaboom(PG_FUNCTION_ARGS)
{
	char *op = TextDatumGetCString(PG_GETARG_DATUM(0));

	/* special gating function check; will abort if everything isn't allowed */
	validate_we_can_blow_up_things();

	/* now check how we want to blow things up ... */

	if (!pg_strcasecmp(op, "fill-pgdata")) {
		fill_disk_at_path(pgdata_path, NULL);
		PG_RETURN_BOOL(1);
	} else if (!pg_strcasecmp(op, "fill-pgwal")) {
		fill_disk_at_path(pgdata_path, "pg_wal");
		PG_RETURN_BOOL(1);
	} else if (!pg_strcasecmp(op, "restart")) {
		validate_we_can_restart();
		restart_database();
		PG_RETURN_BOOL(1);
	} else if (!pg_strcasecmp(op, "segfault")) {
		volatile char *segfault = NULL;
		*segfault = '\0';
		PG_RETURN_BOOL(1);
	} else if (!pg_strcasecmp(op, "signal")) {
		int signal = SIGKILL;

		kill(PostmasterPid, signal);
		PG_RETURN_BOOL(1);
	} else if (!pg_strcasecmp(op, "rm-pgdata")) {
		command_with_path("/bin/rm -Rf %s", pgdata_path, false);
		PG_RETURN_BOOL(1);
	} else {
		ereport(NOTICE, errmsg("unrecognized operation: '%s'", op),
				errhint("must be one of 'fill-pgdata', 'fill-pgwal', 'rm-pgdata', 'segfault' or 'signal'"));
	}

	/* will only return false if we don't recognize the method of destruction or if something failed to fail */
	PG_RETURN_BOOL(0);
}

static void validate_we_can_blow_up_things() {
#ifdef WIN32
	/* bail out on windows */
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("function not supported on Windows (aren't things already broken enough?)")));
#endif

	/* check that we are running as a superuser */
	if (!session_auth_is_superuser)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must run this function as a superuser")));

	/* check disclaimer for matching value */
	if (!disclaimer || strcmp(disclaimer, PG_KABOOM_DISCLAIMER))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("for safety, pg_kaboom.disclaimer must be explicitly set to '%s'",
						PG_KABOOM_DISCLAIMER)));
}

void load_pgdata_path() {
	if (!pgdata_path) {
		MemoryContext oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		pgdata_path = pstrdup(GetConfigOptionByName("data_directory", NULL, false));
		MemoryContextSwitchTo(oldcontext);

		if (!pgdata_path || !strlen(pgdata_path))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("data directory not found")));
	}
}

#define fill_disk_format "/bin/dd if=/dev/zero of=%s/pg_kaboom_space_filler bs=1m"
static void fill_disk_at_path(char *path, char *subpath) {
	/* we control the callers, so path will always be non-null */

	/* if subpath is set, append to original path */
	if (subpath && *subpath) {
		int len = strlen(path);
		char *p = palloc(len + strlen(subpath) + 2); /* separator + newline */
		strcpy(p, path);
		p[len] = '/';
		strcpy(p + len + 1, subpath);
		path = p;
	}

	/* ensure path is an actual directory */
	struct stat buf;
	if (stat(path, &buf) < 0 || !S_ISDIR(buf.st_mode) || access(path, W_OK) < 0)
        ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("'%s' is not a writable directory", path)));

	command_with_path(fill_disk_format, path, false);
}

/* helper to run a command with a path substitute */
static void command_with_path(char *template, char *path, bool detach) {
	/* sanity-check our path here ... */
	if (!path || !*path)
		ereport(ERROR, errmsg("can't run with empty path"));

	if (path[0] != '/')
		ereport(ERROR, errmsg("cowardly not running with relative path"));

	command_with_path_internal(template, path, "", detach);
}

static void command_with_path_internal(char *template, char *arg1, char *arg2, bool detach) {
	/* even when crashing things, proper memory offsets are still classy; note we do waste a byte or
	   two (with '%s'), which when filling up entire disks is a venial sin at best */

	char *command = palloc(strlen(template) + strlen(arg1) + strlen(arg2));
	sprintf(command, template, arg1, arg2);
	ereport(NOTICE, errmsg("%srunning command: '%s'", (execute ? "" : "(dry-run) "), command));
	if (execute) {
		if (detach) {
			/* this is yucky, and probably not that good, however it appears to work */
			daemon(0,0);		/* deprecated warnings, but appears to function */

			if (fork())			/* extra fork needed due to only one level of fork() + setsid() */
				proc_exit(0);	/* exit cleanly for pg -- such that it matters */
			setsid();
			system(command);
		} else
			system(command);
	}
}

static void validate_we_can_restart() {
	/* check and error out early if it looks like we can't force a restart */

	/* for now do nothing */
}

static void restart_database() {
	/* run pg_ctl to restart this database cluster */

	/* it is definitely possible this will not work in all cases (systemd overrides, etc) */
	/* TODO: read/parse /proc invocation of postmaster and just issue that instead? */

	/* for now, we will just try to run pg_ctl -D $pgdata restart -m fast */
	char *command = "bash -c 'kill -9 %s;" PGBINDIR "/pg_ctl -D %s start -l /tmp/pg_kaboom_startup.log'";
	char postmaster_pid[10];

	snprintf(postmaster_pid, 10, "%d", PostmasterPid);
	command_with_path_internal(command, postmaster_pid, pgdata_path, true);
}
