#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/numeric.h"
#include "utils/jsonb.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "storage/ipc.h"
#include "pgstat.h"

#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

#define PG_KABOOM_DISCLAIMER "I can afford to lose this data and server"

/* function signature for the weapon implementation; argument is static so can expose multiple weapons with same function */
typedef void (*wpn_impl)(char *arg, Jsonb *payload);

typedef struct Weapon {
	char *wpn_name;
	wpn_impl wpn_impl;
	char *wpn_arg;
	char *wpn_desc;
} Weapon;

/* weapon prototypes */
static void wpn_special(char *arg, Jsonb *payload);
static void wpn_break_archive(char *arg, Jsonb *payload);
static void wpn_fill_log();
static void wpn_fill_pgdata();
static void wpn_fill_pgwal();
static void wpn_restart();
static void wpn_segfault();
static void wpn_signal(char *arg, Jsonb *payload);
static void wpn_rm_pgdata();
static void wpn_xact_wrap();

Weapon weapons[] = {
	/* "special" weapons */
	{ "random"			, &wpn_special			, "random" , "select a random non-special weapon" },
	{ "null"			, &wpn_special			, "null"   , "noop" },

	{ "break-archive"	, &wpn_break_archive	, NULL, "force archive failures" },
	{ "fill-log"		, &wpn_fill_log			, NULL, "use all the space in the log directory" },
	{ "fill-pgdata"		, &wpn_fill_pgdata		, NULL, "use all the space in the pgdata directory" },
	{ "fill-pgwal"		, &wpn_fill_pgwal		, NULL, "use all the space in the pg_wal directory" },
	{ "restart"			, &wpn_restart			, NULL, "force an immediate restart" },
	{ "segfault"		, &wpn_segfault			, NULL, "segfault inside a backend process" },
	{ "signal"			, &wpn_signal			, NULL, "send a signal to the postmaster (KILL by default)" },
	{ "rm-pgdata"		, &wpn_rm_pgdata		, NULL, "remove the pgdata directory" },
	{ "xact-wrap"		, &wpn_xact_wrap		, NULL, "force wraparound autovacuum" },
	{ NULL, NULL, NULL, NULL }
};

#define NUM_WEAPONS (sizeof(weapons)/sizeof(Weapon) - 1)

/* global variables */
static char *disclaimer;
static char *pgdata_path = NULL;
static bool execute = false;

/* sanity/utility routines */
static void validate_we_can_blow_up_things();
static void validate_we_can_restart();
static void restart_database();
static void load_pgdata_path();
static void fill_disk_at_path(char *path, char *subpath);
static void command_with_path(char *command, char *path, bool detach);
static void command_with_path_internal(char *command, char *arg1, char *arg2, bool detach);
static void force_settings_and_restart(char **setting, char **value);
static char *quoted_string(char * setting);
static char *missing_weapon_hint();
static char *simple_get_json_str(Jsonb *in, char *key);
static int simple_get_json_int(Jsonb *in, char *key);
static pid_t find_random_pid_of_type(char *type);

/* constants snarfed from postmaster.c; no include */
#define BACKEND_TYPE_NORMAL		0x0001	/* normal backend */
#define BACKEND_TYPE_AUTOVAC	0x0002	/* autovacuum worker process */
#define BACKEND_TYPE_WALSND		0x0004	/* walsender process */
#define BACKEND_TYPE_BGWORKER	0x0008	/* bgworker process */
#define BACKEND_TYPE_ALL		0x000F	/* OR of all the above */


PG_MODULE_MAGIC;

void _PG_init(void);
void _PG_fini(void);

Datum pg_kaboom(PG_FUNCTION_ARGS);
Datum pg_kaboom_arsenal(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_kaboom);
PG_FUNCTION_INFO_V1(pg_kaboom_arsenal);

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

	DefineCustomStringVariable("pg_kaboom.saved_archive_command",
							   gettext_noop("Storage for the old archive_command if we have replaced this one"),
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

#define UNKNOWN_HINT_MESSAGE_PREFIX "must be one of: "

Datum pg_kaboom(PG_FUNCTION_ARGS)
{
	char *op = TextDatumGetCString(PG_GETARG_DATUM(0));
	Jsonb *payload = NULL;
	Weapon *weapon = weapons;

	/* special gating function check; will abort if everything isn't allowed */
	validate_we_can_blow_up_things();

	/* check for payload */
	if (!PG_ARGISNULL(1))
		payload = PG_GETARG_JSONB_P(1);

	/* now check how we want to blow things up; linear search for matching name ... */
	while (weapon->wpn_name && pg_strcasecmp(weapon->wpn_name, op) != 0)
		weapon++;

	if (weapon->wpn_name) {
		/* we matched a weapon name */
		weapon->wpn_impl(weapon->wpn_arg, payload);
		PG_RETURN_BOOL(1);
	} else {
		ereport(NOTICE, errmsg("unrecognized operation: '%s'", op), errhint("%s", missing_weapon_hint()));
		PG_RETURN_BOOL(0);
	}
}

static char *missing_weapon_hint() {
	char *hint, *p;
	int i;
	size_t weapon_size = 0;
	Weapon *weapon;

	/* calculate the sum of all of the weapon names */
	weapon = weapons;
	while (weapon->wpn_name) {
		weapon_size += strlen(weapon->wpn_name);
		weapon++;
	}

	/* leading text, the word "or " and trailing newline,
	   additional padding for formatting - 4 bytes per, quote quote comma space */
	weapon_size += sizeof(UNKNOWN_HINT_MESSAGE_PREFIX) + 4 + NUM_WEAPONS * 4;

	/* now allocate the message buffer */
	p = hint = palloc(weapon_size);

	/* start with the hint prefix */
	p = stpcpy(p, UNKNOWN_HINT_MESSAGE_PREFIX);

	/* do our individual copy now of each weapon name, stopping before the last one for the "OR" */
	for (i = 0; i < NUM_WEAPONS - 1; i++) {
		*p++ = '\'';
		p = stpcpy(p, weapons[i].wpn_name);
		*p++ = '\'';
		if (i != NUM_WEAPONS - 2)
			*p++ = ',';
		*p++ = ' ';
	}

	/* final item; only do the " or " if we have more than one */
	if (NUM_WEAPONS > 1) {
		p = stpcpy(p, "or ");
	}

	*p++ = '\'';
	p = stpcpy(p, weapons[NUM_WEAPONS - 1].wpn_name);
	*p++ = '\'';
	*p++ = '.';
	*p++ = '\0';

	return hint;
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

static void load_pgdata_path() {
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
	struct stat buf;

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

	/* for now, we will just force an immediate shutdown and then run pg_ctl -D $pgdata start */
	char *command = "bash -c 'kill -9 %s; sleep 1; %s -D %s start -l /tmp/pg_kaboom_startup.log'";
	char postmaster_pid[10];
	char pg_ctl_path[MAXPGPATH];

	if (find_other_exec(my_exec_path, "pg_ctl", PG_BACKEND_VERSIONSTR,
						pg_ctl_path) < 0)
		ereport(FATAL,
				(errmsg("%s: could not locate matching pg_ctl executable",
						my_exec_path)));

	snprintf(postmaster_pid, 10, "%d", PostmasterPid);
	command_with_path_internal(command, postmaster_pid, pgdata_path, true);
}

static void force_settings_and_restart(char **settings, char **values) {
	char sql[255];
	List *raw_parsetree_list;
	ListCell *lc1;

	validate_we_can_restart();

	while (*settings && *values) {
		char *setting = settings[0];
		char *value = values[0];

		settings++;
		values++;

		/* tried using ALTER SYSTEM directly via SPI, but won't run in a function block */
		/* so we are trying to hack the parser and invoke directly */

		snprintf(sql, 255, "ALTER SYSTEM SET %s = %s", setting, value);

		raw_parsetree_list = pg_parse_query((const char*)sql);

		if (raw_parsetree_list && list_length(raw_parsetree_list) == 1) {
			foreach(lc1, raw_parsetree_list) {
				RawStmt    *parsetree = lfirst_node(RawStmt, lc1);
				AlterSystemSetConfigFile((AlterSystemStmt*)parsetree->stmt);
			}
		} else {
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("error running ALTER SYSTEM")));
		}
	}

	sleep(1);
	restart_database();
}

static char *quoted_string (char *setting) {
	size_t size = strlen(setting) + 3;
	char *qstring = palloc(size); /* start quote, end quote, newline */
	snprintf(qstring, size, "'%s'", setting);

	return qstring;
}

/* simple handlers for pulling expected values from JSON type */
/* returns NULL if missing, or -1 if an int */
static char *simple_get_json_str(Jsonb *in, char *key) {
	Assert(in != NULL);
	Assert(key != NULL);
	Assert(JB_ROOT_IS_OBJECT(in));

	JsonbValue *val = getKeyJsonValueFromContainer(&in->root, key, strlen(key), NULL);
	char *str = NULL;

	/* check if it is a simple scalar value, which it should be */
	if (val->type != jbvString) {
		ereport(ERROR, errmsg("expected string type"));
		return NULL;
	}

	str = palloc(val->val.string.len);
	strncpy(str, val->val.string.val, val->val.string.len);
	return str;
}

static int simple_get_json_int(Jsonb *in, char *key) {
	Assert(in != NULL);
	Assert(key != NULL);
	Assert(JB_ROOT_IS_OBJECT(in));

	JsonbValue *val = getKeyJsonValueFromContainer(&in->root, key, strlen(key), NULL);
	char *str;
	int ret;

	/* check if it is a simple scalar value, which it should be */
	if (val->type != jbvNumeric)
		ereport(ERROR, errmsg("expected integer type"));

	str = numeric_normalize(val->val.numeric);

	if (str && parse_int(str, &ret, 0, NULL))
		return ret;

	ereport(ERROR, errmsg("expected integer type"));

	return -1;
}

/* find a backend of the given type randomly; if picking a client backend, excludes this specific
   backend for obvious reasons.  returns the pid of the process or 0 if not found */

static pid_t find_random_pid_of_type(char *type) {
	int i,
		startIdx,
		num_procs = pgstat_fetch_stat_numbackends(),
		backend_type;
	pid_t pid = 0;
	bool is_first = true;

	/* first calculate the backend_type based on "type" param */
	backend_type =
		!pg_strcasecmp("backend", type) ? BACKEND_TYPE_NORMAL :
		!pg_strcasecmp("autovac", type) ? BACKEND_TYPE_AUTOVAC :
		!pg_strcasecmp("walsender", type) ? BACKEND_TYPE_WALSND :
		!pg_strcasecmp("bgworker", type) ? BACKEND_TYPE_BGWORKER :
		0
		;

	/* TODO: add Auxilary Procs handling to allow you to target them too */
	if (!backend_type)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("can't find backend of type %s", type)));

	/* pick a start index based on a random entry point into the ProcArray */
	i = startIdx = random() % num_procs;

	/* do a linear wrapping search through the array starting at the random offset */
	for (i = startIdx; i != startIdx || is_first; i = ((i + 1) >= num_procs ? 0 : i + 1), is_first = false) {
		PgBackendStatus *st = pgstat_fetch_stat_beentry(i);

		if (st && st->st_procpid > 0 && st->st_procpid != MyProcPid) {
			/* check for correct backend type and exit loop if so */
			if (st->st_backendType == backend_type) {
				pid = st->st_procpid;
				break;
			}
		}
	}

	return pid;
}

/* Weapon definitions */

static void wpn_special(char *arg, Jsonb *payload) {
	/* this is a "special" metaweapon, not a weapon itself */
	if (!pg_strcasecmp(arg, "random")) {
		int wpn_idx;

		do {
			/* doesn't need to be secure, just pseudo-random is fine */
			wpn_idx = rand() % NUM_WEAPONS;

			/* exclude "null" and "random" from random selection */
		} while (!strcmp(weapons[wpn_idx].wpn_name, "null") ||
				 !strcmp(weapons[wpn_idx].wpn_name, "random"));

		ereport(NOTICE, errmsg("deviously selecting the random weapon '%s'",
							   weapons[wpn_idx].wpn_name));

		weapons[wpn_idx].wpn_impl(weapons[wpn_idx].wpn_arg, payload);
	} else if (!pg_strcasecmp(arg, "null")) {
		ereport(NOTICE, errmsg("intentionally doing nothing"));
	}
}

static void wpn_break_archive(char *arg, Jsonb *payload) {
	char *bad_archive_command = payload ? simple_get_json_str(payload, "archive_command") : "/usr/bin/false";
	char *archive_command = GetConfigOptionByName("archive_command", NULL, false);
	char *settings[] = { "archive_mode", "archive_command", "pg_kaboom.saved_archive_command", NULL };
	char *values[] = { "on", quoted_string(bad_archive_command), quoted_string(archive_command), NULL };

	force_settings_and_restart(settings, values);
}

static void wpn_fill_log() {
	char *log_destination = GetConfigOptionByName("log_destination", NULL, false);
	char *log_directory = GetConfigOptionByName("log_directory", NULL, false);

	if (pg_strcasecmp(log_destination, "stderr") || !*log_directory)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("can only fill up log_directory if stderr and set")));

	/* if an absolute path, just use that, otherwise append to the data directory */
	if (*log_directory == '/')
		fill_disk_at_path(log_directory, NULL);
	else
		fill_disk_at_path(pgdata_path, log_directory);
}

static void wpn_fill_pgdata() {
	fill_disk_at_path(pgdata_path, NULL);
}

static void wpn_fill_pgwal() {
	fill_disk_at_path(pgdata_path, "pg_wal");
}

static void wpn_restart() {
	validate_we_can_restart();
	restart_database();
}

static void wpn_segfault() {
	volatile char *segfault = NULL;
	*segfault = '\0';
}

static void wpn_signal(char *arg, Jsonb *payload) {
	int sig = SIGKILL;
	pid_t sig_pid = PostmasterPid;

	if (payload) {
		int raw_sig;
		/* maybe pull out a signal and a backend to target */
		char *type = simple_get_json_str(payload, "type");

		/* look for the pid of a random backend of the given type */
		if (type) {
			sig_pid = find_random_pid_of_type(type);
			if (!sig_pid)
				ereport(NOTICE, errmsg("couldn't find pid of type '%s'", type));
		}
		raw_sig = simple_get_json_int(payload, "signal");
		if (raw_sig)
			sig = raw_sig;
	}

	kill(sig_pid, sig);
}

static void wpn_rm_pgdata() {
	command_with_path("/bin/rm -Rf %s", pgdata_path, false);
}

static void wpn_xact_wrap() {
	char *settings[] = { "autovacuum_freeze_max_age", NULL };
	char *values[] = { "100000", NULL };

	force_settings_and_restart(settings, values);
}


/* SRF to return information about the available weapons */
Datum pg_kaboom_arsenal(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	Weapon *weapon = weapons;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	while (weapon->wpn_name)
	{
		/* for each row */
		Datum		values[2];
		bool		nulls[2];

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		values[0] = CStringGetTextDatum(weapon->wpn_name);
		values[1] = CStringGetTextDatum(weapon->wpn_desc);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		weapon++;
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}
