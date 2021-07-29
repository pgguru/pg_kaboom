#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/builtins.h"

#include <signal.h>

#define PG_KABOOM_DISCLAIMER "I can afford to lose this data and server"

static char *disclaimer;

static void validate_we_can_blow_up_things();
static char *get_pgdata_path();

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
}

void _PG_fini(void)
{
	/* ... C code here at time of extension unloading ... */
}

Datum pg_kaboom(PG_FUNCTION_ARGS)
{
	char *op = TextDatumGetCString(PG_GETARG_DATUM(0));
	char *pgdata_path = get_pgdata_path();

	/* special gating function check; will abort if everything isn't allowed */
	validate_we_can_blow_up_things();

	/* now check how we want to blow things up ... */
	
	if (!pg_strcasecmp(op, "segfault")) {
		volatile char *segfault = NULL;
		*segfault = '\0';
	} else if (!pg_strcasecmp(op, "signal")) {
		int signal = SIGKILL;
		
		kill(PostmasterPid, signal);
	} else if (!pg_strcasecmp(op, "rm-pgdata")) {
		/* even when crashing things, proper memory offsets are still classy */
		char *command = palloc(strlen(pgdata_path) + 13);

		sprintf(command, "/bin/rm -Rf %s", pgdata_path);
		system(command);

		PG_RETURN_BOOL(1);
	} else {
		ereport(NOTICE, errmsg("unrecognized operation: '%s'", op), errhint("must be one of 'rm-pgdata', 'segfault' or 'signal'"));
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

static char *get_pgdata_path() {
	char *pgdata_path = GetConfigOptionByName("data_directory", NULL, false);

	if (!pgdata_path || !strlen(pgdata_path))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("data directory not found")));

	return pgdata_path;
}

