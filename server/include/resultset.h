#ifndef _RESULTSET_H
#define _RESULTSET_H
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
 * @file resultset.h The MaxScale generic result set mechanism
 *
 * @verbatim
 * Revision History
 *
 * Date         Who             Description
 * 17/02/15     Mark Riddoch    Initial implementation
 *
 * @endverbatim
 */
#include <dcb.h>


/**
 * Column types
 */
typedef enum
{
    COL_TYPE_VARCHAR = 0x0f,
    COL_TYPE_VARSTRING = 0xfd
} RESULT_COL_TYPE;

/**
 * The result set column definition. Each result set has an order linked
 * list of column definitions.
 */
typedef struct resultcolumn
{
    char *name;                /*< Column name */
    int len;                   /*< Column length */
    RESULT_COL_TYPE type;      /*< Column type */
    struct resultcolumn *next; /*< next column */
} RESULT_COLUMN;

/**
 * A representation of a row within a result set.
 */
typedef struct resultrow
{
    int n_cols; /*< No. of columns in row */
    char **cols; /*< The columns themselves */
} RESULT_ROW;

struct resultset;

/**
 * Type of callback function used to supply each row
 */
typedef RESULT_ROW * (*RESULT_ROW_CB)(struct resultset *, void *);

/**
 * The representation of the result set itself.
 */
typedef struct resultset
{
    int n_cols;             /*< No. of columns */
    RESULT_COLUMN *column;  /*< Linked list of column definitions */
    RESULT_ROW_CB fetchrow; /*< Fetch a row for the result set */
    void *userdata;         /*< User data for the fetch row call */
} RESULTSET;

extern RESULTSET *resultset_create(RESULT_ROW_CB, void *);
extern void resultset_free(RESULTSET *);
extern int resultset_add_column(RESULTSET *, char *, int, RESULT_COL_TYPE);
extern void resultset_column_free(RESULT_COLUMN *);
extern RESULT_ROW *resultset_make_row(RESULTSET *);
extern void resultset_free_row(RESULT_ROW *);
extern int resultset_row_set(RESULT_ROW *, int, char *);
extern void resultset_stream_mysql(RESULTSET *, DCB *);
extern void resultset_stream_json(RESULTSET *, DCB *);

#endif
