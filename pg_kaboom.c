#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/builtins.h"

#include <signal.h>

#define PG_KABOOM_DISCLAIMER "I can lose this data"

static char *disclaimer;

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
	text *param = PG_GETARG_TEXT_PP(0);
	char *op = text_to_cstring(param);

	/* first check disclaimer for matching value */
	if (!disclaimer || strcmp(disclaimer, PG_KABOOM_DISCLAIMER)) {
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("for safety, pg_kaboom.disclaimer must be explicitly set to '%s'",
						PG_KABOOM_DISCLAIMER)));
	}

	/* now check how we want to blow things up ... */
	
	if (!pg_strcasecmp(op, "segfault")) {
		char *segfault = NULL;
		*segfault = '\0';
	} else if (!pg_strcasecmp(op, "signal")) {
		int signal = SIGKILL;
		
		kill(PostmasterPid, signal);
	} else {
		ereport(NOTICE, errmsg("unrecognized operation: '%s'", op));
	}

	/* will only return false if we don't recognize the method of destruction or if something failed to fail */
	PG_RETURN_BOOL(0);
}
