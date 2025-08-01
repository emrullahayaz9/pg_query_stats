/* pg_query_stats.c - PostgreSQL Query Performance Monitor Extension */

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "executor/executor.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"
#include "utils/guc.h"
#include "storage/ipc.h"
#include "nodes/pg_list.h"

PG_MODULE_MAGIC;

/* GUC Variables */
static bool pgqs_enabled = true;
static int pgqs_max_entries = 100;
static double pgqs_min_duration = 0.0;
#define MAX_QUERY_LENGTH 1024

/* Query Stat Entry */
typedef struct QueryStatEntry {
    char query_text[MAX_QUERY_LENGTH];
    uint64 calls;
    double total_time;
    double min_time;
    double max_time;
} QueryStatEntry;

/* Shared State */
typedef struct pgqsSharedState {
    LWLock *lock;
    int num_entries;
    QueryStatEntry entries[FLEXIBLE_ARRAY_MEMBER];
} pgqsSharedState;

static pgqsSharedState *shared_state = NULL;

/* Executor hooks */
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;

/* Backend-local query timing list */
typedef struct {
    QueryDesc *query;
    TimestampTz start_time;
} pgqsQueryEntry;

static List *query_times_list = NIL;

/* Function prototypes */
static void pgqs_shmem_startup(void);
static void pgqs_shmem_request(void);
static char *pgqs_normalize_query(const char *query);
static void pgqs_update_stats(const char *query, double duration);
static void pgqs_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void pgqs_ExecutorFinish(QueryDesc *queryDesc);

/* SQL-callable functions */
PG_FUNCTION_INFO_V1(pg_query_stats);
PG_FUNCTION_INFO_V1(pg_query_stats_reset);

/* Shared memory initialization */
void _PG_init(void) {
    if (!IsPostmasterEnvironment)
        return;

    if (!process_shared_preload_libraries_in_progress) {
        elog(WARNING, "pg_query_stats: Must be loaded via shared_preload_libraries");
        return;
    }

    DefineCustomBoolVariable("pg_query_stats.enabled",
                             "Enable query statistics collection",
                             NULL,
                             &pgqs_enabled,
                             true,
                             PGC_SUSET,
                             0,
                             NULL, NULL, NULL);

    DefineCustomIntVariable("pg_query_stats.max_entries",
                            "Maximum number of queries to track",
                            NULL,
                            &pgqs_max_entries,
                            100,
                            10,
                            10000,
                            PGC_POSTMASTER,
                            0,
                            NULL, NULL, NULL);

    DefineCustomRealVariable("pg_query_stats.min_duration",
                             "Minimum query duration to track (ms)",
                             NULL,
                             &pgqs_min_duration,
                             0.0,
                             0.0,
                             1000000.0,
                             PGC_SUSET,
                             0,
                             NULL, NULL, NULL);

    shmem_request_hook = pgqs_shmem_request;
    shmem_startup_hook = pgqs_shmem_startup;

    prev_ExecutorStart = ExecutorStart_hook;
    ExecutorStart_hook = pgqs_ExecutorStart;

    prev_ExecutorFinish = ExecutorFinish_hook;
    ExecutorFinish_hook = pgqs_ExecutorFinish;

    elog(LOG, "pg_query_stats: Hooks registered - Start=%p, Finish=%p", 
         pgqs_ExecutorStart, pgqs_ExecutorFinish);
}

/* Shared memory request */
static void pgqs_shmem_request(void) {
    RequestAddinShmemSpace(offsetof(pgqsSharedState, entries) +
                           (pgqs_max_entries * sizeof(QueryStatEntry)));
    RequestNamedLWLockTranche("pg_query_stats", 1);
}

/* Shared memory startup */
static void pgqs_shmem_startup(void) {
    bool found;

    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

    shared_state = ShmemInitStruct("pg_query_stats_state",
                                   offsetof(pgqsSharedState, entries) +
                                   (pgqs_max_entries * sizeof(QueryStatEntry)),
                                   &found);

    if (!shared_state)
        elog(ERROR, "pg_query_stats: could not allocate shared memory");

    shared_state->lock = &(GetNamedLWLockTranche("pg_query_stats"))->lock;

    if (!found) {
        shared_state->num_entries = 0;
        memset(shared_state->entries, 0, pgqs_max_entries * sizeof(QueryStatEntry));
        elog(LOG, "pg_query_stats: initialized shared memory");
    }

    LWLockRelease(AddinShmemInitLock);
}

/* Query normalization (not heavily used here, could be improved) */
static char *pgqs_normalize_query(const char *query) {
    char *normalized;
    bool in_quote = false;
    bool in_dollar = false;
    int j = 0;
    int i;

    normalized = palloc(strlen(query) + 1);

    for (i = 0; query[i]; i++) {
        if (query[i] == '\'' && !in_dollar)
            in_quote = !in_quote;
        else if (query[i] == '$' && !in_quote)
            in_dollar = !in_dollar;

        if ((in_quote || in_dollar) && (isdigit(query[i]) || query[i] == '?')) {
            if (j == 0 || normalized[j - 1] != '?')
                normalized[j++] = '?';
        } else {
            normalized[j++] = query[i];
        }
    }
    normalized[j] = '\0';

    return normalized;
}

/* Update shared stats */
static void pgqs_update_stats(const char *query, double duration) {
    int i;
    char normalized[MAX_QUERY_LENGTH];

    if (!shared_state) {
        elog(WARNING, "pg_query_stats: shared_state is NULL");
        return;
    }

    strncpy(normalized, query, MAX_QUERY_LENGTH - 1);
    normalized[MAX_QUERY_LENGTH - 1] = '\0';

    LWLockAcquire(shared_state->lock, LW_EXCLUSIVE);

    for (i = 0; i < shared_state->num_entries; i++) {
        if (strcmp(shared_state->entries[i].query_text, normalized) == 0) {
            shared_state->entries[i].calls++;
            shared_state->entries[i].total_time += duration;
            if (duration < shared_state->entries[i].min_time)
                shared_state->entries[i].min_time = duration;
            if (duration > shared_state->entries[i].max_time)
                shared_state->entries[i].max_time = duration;
            LWLockRelease(shared_state->lock);
            return;
        }
    }

    if (shared_state->num_entries < pgqs_max_entries) {
        QueryStatEntry *entry = &shared_state->entries[shared_state->num_entries];
        strncpy(entry->query_text, normalized, MAX_QUERY_LENGTH);
        entry->calls = 1;
        entry->total_time = duration;
        entry->min_time = duration;
        entry->max_time = duration;
        shared_state->num_entries++;
        elog(LOG, "pg_query_stats: added new entry for: %s", normalized);
    }

    LWLockRelease(shared_state->lock);
}

/* ExecutorStart: store start time */
static void pgqs_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
    pgqsQueryEntry *entry;

    if (prev_ExecutorStart)
        prev_ExecutorStart(queryDesc, eflags);
    else
        standard_ExecutorStart(queryDesc, eflags);

    if (!pgqs_enabled || !queryDesc->sourceText)
        return;

    if (strstr(queryDesc->sourceText, "pg_query_stats") != NULL)
        return;

    entry = palloc(sizeof(pgqsQueryEntry));
    entry->query = queryDesc;
    entry->start_time = GetCurrentTimestamp();
    query_times_list = lappend(query_times_list, entry);

    elog(LOG, "pg_query_stats: stored start time for query: %s", queryDesc->sourceText);
}

/* ExecutorFinish: calculate duration */
static void pgqs_ExecutorFinish(QueryDesc *queryDesc)
{
    ListCell *lc;
    pgqsQueryEntry *entry = NULL;

    if (prev_ExecutorFinish)
        prev_ExecutorFinish(queryDesc);
    else
        standard_ExecutorFinish(queryDesc);

    if (!pgqs_enabled || !queryDesc->sourceText)
        return;

    foreach(lc, query_times_list)
    {
        pgqsQueryEntry *e = (pgqsQueryEntry *) lfirst(lc);
        if (e->query == queryDesc)
        {
            entry = e;
            break;
        }
    }

    if (entry)
    {
        double duration_ms = (double)(GetCurrentTimestamp() - entry->start_time) / 1000.0;

        elog(LOG, "pg_query_stats: query duration: %.3f ms for: %s",
             duration_ms, queryDesc->sourceText);

        if (duration_ms >= pgqs_min_duration)
            pgqs_update_stats(queryDesc->sourceText, duration_ms);

        query_times_list = list_delete_ptr(query_times_list, entry);
        pfree(entry);
    }
    else
    {
        elog(LOG, "pg_query_stats: no start time found for query: %s",
             queryDesc->sourceText);
    }
}

/* pg_query_stats */
Datum pg_query_stats(PG_FUNCTION_ARGS) {
    FuncCallContext *funcctx;
    MemoryContext oldcontext;

    if (SRF_IS_FIRSTCALL()) {
        TupleDesc tupdesc;

        funcctx = SRF_FIRSTCALL_INIT();
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        tupdesc = CreateTemplateTupleDesc(5);
        TupleDescInitEntry(tupdesc, 1, "query_text", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, 2, "calls", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, 3, "total_time", FLOAT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, 4, "min_time", FLOAT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, 5, "max_time", FLOAT8OID, -1, 0);

        funcctx->tuple_desc = BlessTupleDesc(tupdesc);
        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();

    LWLockAcquire(shared_state->lock, LW_SHARED);

    if (funcctx->call_cntr < shared_state->num_entries) {
        Datum values[5];
        bool nulls[5] = {false};
        HeapTuple tuple;
        QueryStatEntry *entry = &shared_state->entries[funcctx->call_cntr];

        values[0] = CStringGetTextDatum(entry->query_text);
        values[1] = Int64GetDatum(entry->calls);
        values[2] = Float8GetDatum(entry->total_time);
        values[3] = Float8GetDatum(entry->min_time);
        values[4] = Float8GetDatum(entry->max_time);

        tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        LWLockRelease(shared_state->lock);
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }

    LWLockRelease(shared_state->lock);
    SRF_RETURN_DONE(funcctx);
}

/* pg_query_stats_reset */
Datum pg_query_stats_reset(PG_FUNCTION_ARGS) {
    LWLockAcquire(shared_state->lock, LW_EXCLUSIVE);
    shared_state->num_entries = 0;
    LWLockRelease(shared_state->lock);

    PG_RETURN_VOID();
}

/* Cleanup hook */
void _PG_fini(void) {
    ExecutorStart_hook = prev_ExecutorStart;
    ExecutorFinish_hook = prev_ExecutorFinish;

    if (query_times_list)
    {
        list_free_deep(query_times_list);
        query_times_list = NIL;
    }
}
