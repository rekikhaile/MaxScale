/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl.
 *
 * Change Date: 2019-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

/**
 * @file qlafilter.c - Quary Log All Filter
 * @verbatim
 *
 * QLA Filter - Query Log All. A primitive query logging filter, simply
 * used to verify the filter mechanism for downstream filters. All queries
 * that are passed through the filter will be written to file.
 *
 * The filter makes no attempt to deal with query packets that do not fit
 * in a single GWBUF.
 *
 * A single option may be passed to the filter, this is the name of the
 * file to which the queries are logged. A serial number is appended to this
 * name in order that each session logs to a different file.
 *
 * Date         Who             Description
 * 03/06/2014   Mark Riddoch    Initial implementation
 * 11/06/2014   Mark Riddoch    Addition of source and match parameters
 * 19/06/2014   Mark Riddoch    Addition of user parameter
 *
 * @endverbatim
 */

#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <filter.h>
#include <modinfo.h>
#include <modutil.h>
#include <skygw_utils.h>
#include <log_manager.h>
#include <time.h>
#include <sys/time.h>
#include <regex.h>
#include <string.h>
#include <atomic.h>
#include <query_classifier.h>

MODULE_INFO info =
{
    MODULE_API_FILTER,
    MODULE_GA,
    FILTER_VERSION,
    "A simple query logging filter"
};

static char *version_str = "V1.1.1";

/** Formatting buffer size */
#define QLA_STRING_BUFFER_SIZE 1024

/*
 * The filter entry points
 */
static FILTER *createInstance(char **options, FILTER_PARAMETER **);
static void *newSession(FILTER *instance, SESSION *session);
static void closeSession(FILTER *instance, void *session);
static void freeSession(FILTER *instance, void *session);
static void setDownstream(FILTER *instance, void *fsession, DOWNSTREAM *downstream);
static int routeQuery(FILTER *instance, void *fsession, GWBUF *queue);
static void diagnostic(FILTER *instance, void *fsession, DCB *dcb);


static FILTER_OBJECT MyObject =
{
    createInstance,
    newSession,
    closeSession,
    freeSession,
    setDownstream,
    NULL, // No Upstream requirement
    routeQuery,
    NULL, // No client reply
    diagnostic,
};

/**
 * A instance structure, the assumption is that the option passed
 * to the filter is simply a base for the filename to which the queries
 * are logged.
 *
 * To this base a session number is attached such that each session will
 * have a unique name.
 */
typedef struct
{
    int sessions; /* The count of sessions */
    char *filebase; /* The filename base */
    char *source; /* The source of the client connection */
    char *userName; /* The user name to filter on */
    char *match; /* Optional text to match against */
    regex_t re; /* Compiled regex text */
    char *nomatch; /* Optional text to match against for exclusion */
    regex_t nore; /* Compiled regex nomatch text */
    int loggingMode; /*rekik variable used to determine logging mode*/
} QLA_INSTANCE;

static const int LOGGING_MODE_PER_SESSION = 0;
static const int LOGGING_MODE_SINGLE_FILE = 1;
static const int LOGGING_MODE_PER_DATABASE = 2;

/**
 * The session structure for this QLA filter.
 * This stores the downstream filter information, such that the
 * filter is able to pass the query on to the next filter (or router)
 * in the chain.
 *
 * It also holds the file descriptor to which queries are written.
 */
typedef struct
{
    DOWNSTREAM down;
    char *filename;
    FILE *fp;
    int active;
    char *user;
    char *remote;
    int sessionID; /*rekik track the client's session ID for logging in a single file*/
    int databaseNameFollows; /*rekik flags that the next statement will contain the name of the database being switched to*/
} QLA_SESSION;

/**
 * Implementation of the mandatory version entry point
 *
 * @return version string of the module
 */
char *
version()
{
    return version_str;
}

/**
 * The module initialisation routine, called when the module
 * is first loaded.
 * @see function load_module in load_utils.c for explanation of lint
 */
/*lint -e14 */
void
ModuleInit()
{
}
/*lint +e14 */

/**
 * The module entry point routine. It is this routine that
 * must populate the structure that is referred to as the
 * "module object", this is a structure with the set of
 * external entry points for this module.
 *
 * @return The module object
 */
FILTER_OBJECT *
GetModuleObject()
{
    return &MyObject;
}

/**
 * Create an instance of the filter for a particular service
 * within MaxScale.
 *
 * @param options   The options for this filter
 * @param params    The array of name/value pair parameters for the filter
 *
 * @return The instance data for this new instance
 */
static FILTER *
createInstance(char **options, FILTER_PARAMETER **params)
{
    QLA_INSTANCE *my_instance;
    int i;

    if ((my_instance = malloc(sizeof(QLA_INSTANCE))) != NULL)
    {
        my_instance->source = NULL;
        my_instance->userName = NULL;
        my_instance->match = NULL;
        my_instance->nomatch = NULL;
        my_instance->filebase = NULL;
        my_instance->loggingMode = LOGGING_MODE_PER_DATABASE; //rekik set logging mode
        bool error = false;

        if (params)
        {
            for (i = 0; params[i]; i++)
            {
                if (!strcmp(params[i]->name, "match"))
                {
                    my_instance->match = strdup(params[i]->value);
                }
                else if (!strcmp(params[i]->name, "exclude"))
                {
                    my_instance->nomatch = strdup(params[i]->value);
                }
                else if (!strcmp(params[i]->name, "source"))
                {
                    my_instance->source = strdup(params[i]->value);
                }
                else if (!strcmp(params[i]->name, "user"))
                {
                    my_instance->userName = strdup(params[i]->value);
                }
                else if (!strcmp(params[i]->name, "filebase"))
                {
                    my_instance->filebase = strdup(params[i]->value);
                }
                else if (!filter_standard_parameter(params[i]->name))
                {
                    MXS_ERROR("qlafilter: Unexpected parameter '%s'.",
                              params[i]->name);
                    error = true;
                }
            }
        }

        int cflags = REG_ICASE;

        if (options)
        {
            for (i = 0; options[i]; i++)
            {
                if (!strcasecmp(options[i], "ignorecase"))
                {
                    cflags |= REG_ICASE;
                }
                else if (!strcasecmp(options[i], "case"))
                {
                    cflags &= ~REG_ICASE;
                }
                else if (!strcasecmp(options[i], "extended"))
                {
                    cflags |= REG_EXTENDED;
                }
                else
                {
                    MXS_ERROR("qlafilter: Unsupported option '%s'.",
                              options[i]);
                    error = true;
                }
            }
        }

        if (my_instance->filebase == NULL)
        {
            MXS_ERROR("qlafilter: No 'filebase' parameter defined.");
            error = true;
        }

        my_instance->sessions = 0;
        if (my_instance->match &&
            regcomp(&my_instance->re, my_instance->match, cflags))
        {
            MXS_ERROR("qlafilter: Invalid regular expression '%s'"
                      " for the 'match' parameter.\n",
                      my_instance->match);
            free(my_instance->match);
            my_instance->match = NULL;
            error = true;
        }
        if (my_instance->nomatch &&
            regcomp(&my_instance->nore, my_instance->nomatch, cflags))
        {
            MXS_ERROR("qlafilter: Invalid regular expression '%s'"
                      " for the 'nomatch' parameter.",
                      my_instance->nomatch);
            free(my_instance->nomatch);
            my_instance->nomatch = NULL;
            error = true;
        }

        if (error)
        {
            if (my_instance->match)
            {
                free(my_instance->match);
                regfree(&my_instance->re);
            }

            if (my_instance->nomatch)
            {
                free(my_instance->nomatch);
                regfree(&my_instance->nore);
            }
            free(my_instance->filebase);
            free(my_instance->source);
            free(my_instance->userName);
            free(my_instance);
            my_instance = NULL;
        }
    }
    return (FILTER *) my_instance;
}

/**
 * Associate a new session with this instance of the filter.
 *
 * Create the file to log to and open it.
 *
 * @param instance  The filter instance data
 * @param session   The session itself
 * @return Session specific data for this session
 */
static void *
newSession(FILTER *instance, SESSION *session)
{
    QLA_INSTANCE *my_instance = (QLA_INSTANCE *) instance;
    QLA_SESSION *my_session;
    char *remote, *userName;

    if ((my_session = calloc(1, sizeof(QLA_SESSION))) != NULL)
    {
        if ((my_session->filename = (char *)malloc(strlen(my_instance->filebase) + 20)) == NULL)
        {
            char errbuf[STRERROR_BUFLEN];
            MXS_ERROR("Memory allocation for qla filter "
                      "file name failed due to %d, %s.",
                      errno,
                      strerror_r(errno, errbuf, sizeof(errbuf)));
            free(my_session);
            return NULL;
        }
        my_session->active = 1;

        remote = session_get_remote(session);
        userName = session_getUser(session);
        ss_dassert(userName && remote);

        if ((my_instance->source && remote &&
             strcmp(remote, my_instance->source)) ||
            (my_instance->userName && userName &&
             strcmp(userName, my_instance->userName)))
        {
            my_session->active = 0;
        }

        my_session->user = userName;
        my_session->remote = remote;

        my_session->sessionID = my_instance->sessions; /*rekik saving client's session ID to distinguish clients
                                                            when logging into a single file*/

        my_session->databaseNameFollows = 0; /*rekik initialize flag to false*/


        /*rekik check logging mode and set filename accordingly.
          If logging into a single file, set the filename to
          my_instance->filebase. If logging into multiple files,
          append client session ID to the filename*/
        if (my_instance->loggingMode == LOGGING_MODE_SINGLE_FILE)
        {
            strcpy(my_session->filename, my_instance->filebase);
        }
        else if (my_instance->loggingMode == LOGGING_MODE_PER_SESSION)
        {
            sprintf(my_session->filename, "%s.%d",
                    my_instance->filebase,
                    my_instance->sessions);
        }
        else if (my_instance->loggingMode == LOGGING_MODE_PER_DATABASE)
        {
        	// rekik when logging per database we can only decide which file to open when we see the query
        }

        // Multiple sessions can try to update my_instance->sessions simultaneously
        atomic_add(&(my_instance->sessions), 1);

        if (my_session->active)
        {
            /*rekik append in order to write in one file to log it into a single file*/
            if (my_instance->loggingMode == LOGGING_MODE_SINGLE_FILE)
            {
                my_session->fp = fopen(my_session->filename, "a");
            }
            else if (my_instance->loggingMode == LOGGING_MODE_PER_SESSION)
            {
                my_session->fp = fopen(my_session->filename, "w");
            }
            else if (my_instance->loggingMode == LOGGING_MODE_PER_DATABASE)
            {
            	/*rekik when logging per database we can only decide
            	which file to open when we see the query*/
            }

            /*rekik added check for per-database mode since then we don't
             * open a file yet since we haven't seen queries to get the
             * name of the database yet*/
            if (my_session->fp == NULL && my_instance->loggingMode != LOGGING_MODE_PER_DATABASE)
            {
                char errbuf[STRERROR_BUFLEN];
                MXS_ERROR("Opening output file for qla "
                          "fileter failed due to %d, %s",
                          errno,
                          strerror_r(errno, errbuf, sizeof(errbuf)));
                free(my_session->filename);
                free(my_session);
                my_session = NULL;
            }
        }
    }
    else
    {
        char errbuf[STRERROR_BUFLEN];
        MXS_ERROR("Memory allocation for qla filter failed due to "
                  "%d, %s.",
                  errno,
                  strerror_r(errno, errbuf, sizeof(errbuf)));
    }
    return my_session;
}

/**
 * Close a session with the filter, this is the mechanism
 * by which a filter may cleanup data structure etc.
 * In the case of the QLA filter we simple close the file descriptor.
 *
 * @param instance  The filter instance data
 * @param session   The session being closed
 */
static void

closeSession(FILTER *instance, void *session)
{
    QLA_SESSION *my_session = (QLA_SESSION *) session;

    if (my_session->active && my_session->fp)
    {
        fclose(my_session->fp);
    }
}

/**
 * Free the memory associated with the session
 *
 * @param instance  The filter instance
 * @param session   The filter session
 */
static void
freeSession(FILTER *instance, void *session)
{
    QLA_SESSION *my_session = (QLA_SESSION *) session;

    free(my_session->filename);
    free(session);
    return;
}

/**
 * Set the downstream filter or router to which queries will be
 * passed from this filter.
 *
 * @param instance  The filter instance data
 * @param session   The filter session
 * @param downstream    The downstream filter or router.
 */
static void
setDownstream(FILTER *instance, void *session, DOWNSTREAM *downstream)
{
    QLA_SESSION *my_session = (QLA_SESSION *) session;

    my_session->down = *downstream;
}

/**
 * The routeQuery entry point. This is passed the query buffer
 * to which the filter should be applied. Once applied the
 * query should normally be passed to the downstream component
 * (filter or router) in the filter chain.
 *
 * @param instance  The filter instance data
 * @param session   The filter session
 * @param queue     The query data
 */
static int
routeQuery(FILTER *instance, void *session, GWBUF *queue)
{
    QLA_INSTANCE *my_instance = (QLA_INSTANCE *) instance;
    QLA_SESSION *my_session = (QLA_SESSION *) session;
    char *ptr;
    int length = 0;
    struct tm t;
    struct timeval tv;

    if (my_session->active)
    {
        if (queue->next != NULL)
        {
            queue = gwbuf_make_contiguous(queue);
        }
        if ((ptr = modutil_get_SQL(queue)) != NULL)
        {
            if ((my_instance->match == NULL ||
                 regexec(&my_instance->re, ptr, 0, NULL, 0) == 0) &&
                (my_instance->nomatch == NULL ||
                 regexec(&my_instance->nore, ptr, 0, NULL, 0) != 0))
            {
                char buffer[QLA_STRING_BUFFER_SIZE];
                gettimeofday(&tv, NULL);
                localtime_r(&tv.tv_sec, &t);
                strftime(buffer, sizeof(buffer), "%F %T", &t);

                /*rekik writing to the file in a mode dependent way*/
                if (my_instance->loggingMode == LOGGING_MODE_SINGLE_FILE)
                {
                	/*rekik add session ID to differentiate between
                 	 different client sessions if logging to a single file*/

                		fprintf(my_session->fp, "%s,%s@%s,%s,%d\n", buffer, my_session->user,
                		                				my_session->remote, trim(squeeze_whitespace(ptr)), my_session->sessionID);
                }
                else if (my_instance->loggingMode == LOGGING_MODE_PER_SESSION)
                {
                    fprintf(my_session->fp, "%s,%s@%s,%s\n", buffer, my_session->user,
                            my_session->remote, trim(squeeze_whitespace(ptr)));
                }
                else if (my_instance->loggingMode == LOGGING_MODE_PER_DATABASE)
                {
                	/*rekik if the last query seen was "SELECT DATABASE()" we expect the
                	 * database name now */
					if (my_session->databaseNameFollows == 1)
					{
						if (my_session->fp != NULL)
						{
							fclose(my_session->fp);
						}

						sprintf(my_session->filename, "%s.%s",
								my_instance->filebase,
								trim(squeeze_whitespace(ptr)));

						my_session->fp = fopen(my_session->filename, "a");

						my_session->databaseNameFollows = 0;
					}

					/*rekik look database change operation*/
					if (strcmp(trim(squeeze_whitespace(ptr)), "SELECT DATABASE()") == 0)
					{
						my_session->databaseNameFollows = 1;
					}

					/*rekik if we have a file open we can write to it*/
					if (my_session->fp != NULL)
					{
						fprintf(my_session->fp, "%s,%s@%s,%s,%d\n", buffer, my_session->user,
						        my_session->remote, trim(squeeze_whitespace(ptr)), my_session->sessionID);
					}
                }
            }
            free(ptr);
        }
    }
    /* Pass the query downstream */
    return my_session->down.routeQuery(my_session->down.instance,
                                       my_session->down.session, queue);
}

/**
 * Diagnostics routine
 *
 * If fsession is NULL then print diagnostics on the filter
 * instance as a whole, otherwise print diagnostics for the
 * particular session.
 *
 * @param   instance    The filter instance
 * @param   fsession    Filter session, may be NULL
 * @param   dcb     The DCB for diagnostic output
 */
static void
diagnostic(FILTER *instance, void *fsession, DCB *dcb)
{
    QLA_INSTANCE *my_instance = (QLA_INSTANCE *) instance;
    QLA_SESSION *my_session = (QLA_SESSION *) fsession;

    if (my_session)
    {
        dcb_printf(dcb, "\t\tLogging to file            %s.\n",
                   my_session->filename);
    }
    if (my_instance->source)
    {
        dcb_printf(dcb, "\t\tLimit logging to connections from  %s\n",
                   my_instance->source);
    }
    if (my_instance->userName)
    {
        dcb_printf(dcb, "\t\tLimit logging to user      %s\n",
                   my_instance->userName);
    }
    if (my_instance->match)
    {
        dcb_printf(dcb, "\t\tInclude queries that match     %s\n",
                   my_instance->match);
    }
    if (my_instance->nomatch)
    {
        dcb_printf(dcb, "\t\tExclude queries that match     %s\n",
                   my_instance->nomatch);
    }
}
