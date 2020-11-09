/*-
 * Copyright (c) 2000 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Dieter Baron and Thomas Klausner.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "getopt.h"

int opterr = 1;   /* if error message should be printed */
int optind = 1;   /* index into parent argv vector */
int optopt = '?'; /* character checked for validity */
int optreset;     /* reset getopt */
char* optarg;     /* argument associated with option */

#ifdef _WIN32
size_t getenv_length; /* argument associated with getenv_s() */
#endif

#define BUILD_ASSERT_TYPE(POINTER, TYPE) \
    ((void)sizeof((int)((POINTER) == (TYPE)(POINTER))))
#define CONST_CAST(TYPE, POINTER) \
    (BUILD_ASSERT_TYPE(POINTER, TYPE), (TYPE)(POINTER))
#define IGNORE_FIRST (*options == '-' || *options == '+')
#define PRINT_ERROR \
    ((opterr) && ((*options != ':') || (IGNORE_FIRST && options[1] != ':')))
#ifdef _WIN32
#define IS_POSIXLY_CORRECT \
    (getenv_s(&getenv_length, NULL, 0, "POSIXLY_CORRECT") > 0)
#else
#define IS_POSIXLY_CORRECT (getenv("POSIXLY_CORRECT") != NULL)
#endif
#define PERMUTE (!IS_POSIXLY_CORRECT && !IGNORE_FIRST)
#define IN_ORDER (!IS_POSIXLY_CORRECT && *options == '-')

/* return values */
#define BADCH (int)'?'
#define BADARG                                                           \
    ((IGNORE_FIRST && options[1] == ':') || (*options == ':') ? (int)':' \
                                                              : (int)'?')
#define INORDER (int)1
#define EMSG ""
#define _DIAGASSERT(q) assert(q)

static int getopt_internal(int, char**, const char*);
static int gcd(int, int);
static void permute_args(int, int, int, char**);

static const char* place = EMSG; /* option letter processing */

static int nonopt_start = -1; /* first non option argument (for permute) */
static int nonopt_end = -1;   /* first option after non options (for permute) */

/* Error messages */
static const char recargchar[] = "option requires an argument -- %c";
static const char recargstring[] = "option requires an argument -- %s";
static const char ambig[] = "ambiguous option -- %.*s";
static const char noarg[] = "option doesn't take an argument -- %.*s";
static const char illoptchar[] = "unknown option -- %c";
static const char illoptstring[] = "unknown option -- %s";

static FILE* err_file; /* file to use for error output */

/*
  This is declared to take a `void ' so that the caller is not required
  to include <stdio.h> first.  However, it is really a `FILE ', and the
 * manual page documents it as such.
 */
void err_set_file(void* fp)
{
    if (fp)
        err_file = fp;
    else
        err_file = stderr;
}

void vwarnx(const char* fmt, va_list ap)
{
    if (fmt != NULL)
    {
        if (err_file == 0)
            err_set_file((FILE*)0);
        fprintf(err_file, "getopt_long: ");
        vfprintf(err_file, fmt, ap);
        fprintf(err_file, "\n");
    }
}

void warnx(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwarnx(fmt, ap);
    va_end(ap);
}

/*
 * Compute the greatest common divisor of a and b.
 */
static int gcd(int a, int b)
{
    int c;

    c = a % b;
    while (c != 0)
    {
        a = b;
        b = c;
        c = a % b;
    }

    return b;
}

/*
 * Exchange the block from nonopt_start to nonopt_end with the block
 * from nonopt_end to opt_end (keeping the same order of arguments
 * in each block).
 */
static void permute_args(
    int panonopt_start,
    int panonopt_end,
    int opt_end,
    char** nargv)
{
    int cstart, cyclelen, i, j, ncycle, nnonopts, nopts, pos;
    char* swap;

    _DIAGASSERT(nargv != NULL);

    /*
     * compute lengths of blocks and number and size of cycles
     */
    nnonopts = panonopt_end - panonopt_start;
    nopts = opt_end - panonopt_end;
    ncycle = gcd(nnonopts, nopts);
    cyclelen = (opt_end - panonopt_start) / ncycle;

    for (i = 0; i < ncycle; i++)
    {
        cstart = panonopt_end + i;
        pos = cstart;
        for (j = 0; j < cyclelen; j++)
        {
            if (pos >= panonopt_end)
                pos -= nnonopts;
            else
                pos += nopts;
            swap = nargv[pos];
            nargv[pos] = nargv[cstart];
            nargv[cstart] = swap;
        }
    }
}

/*
 * getopt_internal --
 *  Parse argc/argv argument vector.  Called by user level routines.
 *  Returns -2 if -- is found (can be long option or end of options marker).
 */
static int getopt_internal(int nargc, char** nargv, const char* options)
{
    char* oli; /* option letter list index */
    int optchar;

    _DIAGASSERT(nargv != NULL);
    _DIAGASSERT(options != NULL);

    optarg = NULL;

    if (optind == 0)
        optind = 1;

    if (optreset)
        nonopt_start = nonopt_end = -1;
start:
    if (optreset || !*place)
    {
        /* update scanning pointer */
        optreset = 0;
        if (optind >= nargc)
        {
            /* end of argument vector */
            place = EMSG;
            if (nonopt_end != -1)
            {
                /* do permutation, if we have to */
                permute_args(nonopt_start, nonopt_end, optind, nargv);
                optind -= nonopt_end - nonopt_start;
            }
            else if (nonopt_start != -1)
            {
                /*
                 * If we skipped non-options, set optind
                 * to the first of them.
                 */
                optind = nonopt_start;
            }
            nonopt_start = nonopt_end = -1;
            return -1;
        }
        if ((*(place = nargv[optind]) != '-') || (place[1] == '\0'))
        {
            /* found non-option */
            place = EMSG;
            if (IN_ORDER)
            {
                /*
                 * GNU extension:
                 * return non-option as argument to option 1
                 */
                optarg = nargv[optind++];
                return INORDER;
            }
            if (!PERMUTE)
            {
                /*
                 * if no permutation wanted, stop parsing
                 * at first non-option
                 */
                return -1;
            }
            /* do permutation */
            if (nonopt_start == -1)
                nonopt_start = optind;
            else if (nonopt_end != -1)
            {
                permute_args(nonopt_start, nonopt_end, optind, nargv);
                nonopt_start = optind - (nonopt_end - nonopt_start);
                nonopt_end = -1;
            }
            optind++;
            /* process next argument */
            goto start;
        }
        if (nonopt_start != -1 && nonopt_end == -1)
            nonopt_end = optind;
        if (place[1] && *++place == '-')
        {
            /* found "--" */
            place++;
            return -2;
        }
    }
    if ((optchar = (int)*place++) == (int)':' ||
        (oli = strchr(options + (IGNORE_FIRST ? 1 : 0), optchar)) == NULL)
    {
        /* option letter unknown or ':' */
        if (!*place)
            ++optind;
        if (PRINT_ERROR)
            warnx(illoptchar, optchar);
        optopt = optchar;
        return BADCH;
    }
    if (optchar == 'W' && oli[1] == ';')
    {
        /* -W long-option */
        if (*place)
            return -2;

        if (++optind >= nargc)
        {
            /* no arg */
            place = EMSG;
            if (PRINT_ERROR)
                warnx(recargchar, optchar);
            optopt = optchar;
            return BADARG;
        }
        else
        {
            /* white space */
            place = nargv[optind];
        }
        /*
         * Handle -W arg the same as --arg (which causes getopt to
         * stop parsing).
         */
        return -2;
    }
    if (*++oli != ':')
    {
        /* doesn't take argument */
        if (!*place)
            ++optind;
    }
    else
    {
        /* takes (optional) argument */
        optarg = NULL;
        if (*place)
            /* no white space */
            optarg = CONST_CAST(char*, place);
        else if (oli[1] != ':')
        {
            /* arg not optional */
            if (++optind >= nargc)
            {
                /* no arg */
                place = EMSG;
                if (PRINT_ERROR)
                    warnx(recargchar, optchar);
                optopt = optchar;
                return BADARG;
            }
            else
            {
                optarg = nargv[optind];
            }
        }
        place = EMSG;
        ++optind;
    }
    /* dump back option letter */
    return optchar;
}

#ifdef REPLACE_GETOPT
/*
 * getopt --
 *  Parse argc/argv argument vector.
 *
 * [eventually this will replace the real getopt]
 */
int getopt(nargc, nargv, options) int nargc;
char* const* nargv;
const char* options;
{
    int retval;

    _DIAGASSERT(nargv != NULL);
    _DIAGASSERT(options != NULL);

    retval = getopt_internal(nargc, CONST_CAST(char**, nargv), options);
    if (retval == -2)
    {
        ++optind;
        /*
         * We found an option (--), so if we skipped non-options,
         * we have to permute.
         */
        if (nonopt_end != -1)
        {
            permute_args(nonopt_start, nonopt_end, optind, nargv);
            optind -= nonopt_end - nonopt_start;
        }
        nonopt_start = nonopt_end = -1;
        retval = -1;
    }
    return retval;
}
#endif

/*
 * getopt_long --
 *  Parse argc/argv argument vector.
 */
int getopt_long(
    int nargc,
    char* const* nargv,
    const char* options,
    const struct option* long_options,
    int* idx)
{
    int retval;

#define IDENTICAL_INTERPRETATION(_x, _y)                         \
    (long_options[(_x)].has_arg == long_options[(_y)].has_arg && \
     long_options[(_x)].flag == long_options[(_y)].flag &&       \
     long_options[(_x)].val == long_options[(_y)].val)

    _DIAGASSERT(nargv != NULL);
    _DIAGASSERT(options != NULL);
    _DIAGASSERT(long_options != NULL);
    /* idx may be NULL */

    retval = getopt_internal(nargc, CONST_CAST(char**, nargv), options);
    if (retval == -2)
    {
        char *current_argv, *has_equal;
        size_t current_argv_len;
        int i, ambiguous, match;

        current_argv = CONST_CAST(char*, place);
        match = -1;
        ambiguous = 0;

        optind++;
        place = EMSG;

        if (*current_argv == '\0')
        {
            /*
             * We found an option (--), so if we skipped
             * non-options, we have to permute.
             */
            if (nonopt_end != -1)
            {
                permute_args(
                    nonopt_start,
                    nonopt_end,
                    optind,
                    CONST_CAST(char**, nargv));
                optind -= nonopt_end - nonopt_start;
            }
            nonopt_start = nonopt_end = -1;
            return -1;
        }
        if ((has_equal = strchr(current_argv, '=')) != NULL)
        {
            /* argument found (--option=arg) */
            current_argv_len = (size_t)(has_equal - current_argv);
            has_equal++;
        }
        else
        {
            current_argv_len = strlen(current_argv);
        }

        for (i = 0; long_options[i].name; i++)
        {
            /* find matching long option */
            if (strncmp(current_argv, long_options[i].name, current_argv_len))
                continue;

            if (strlen(long_options[i].name) == (unsigned)current_argv_len)
            {
                /* exact match */
                match = i;
                ambiguous = 0;
                break;
            }
            if (match == -1)
            {
                /* partial match */
                match = i;
            }
            else if (!IDENTICAL_INTERPRETATION(i, match))
                ambiguous = 1;
        }
        if (ambiguous)
        {
            /* ambiguous abbreviation */
            if (PRINT_ERROR)
                warnx(ambig, (int)current_argv_len, current_argv);
            optopt = 0;
            return BADCH;
        }
        if (match != -1)
        {
            /* option found */
            if (long_options[match].has_arg == no_argument && has_equal)
            {
                if (PRINT_ERROR)
                    warnx(noarg, (int)current_argv_len, current_argv);
                if (long_options[match].flag == NULL)
                    optopt = long_options[match].val;
                else
                    optopt = 0;
                return BADARG;
            }
            if (long_options[match].has_arg == required_argument ||
                long_options[match].has_arg == optional_argument)
            {
                if (has_equal)
                    optarg = has_equal;
                else if (long_options[match].has_arg == required_argument)
                {
                    /*
                     * optional argument doesn't use
                     * next nargv
                     */
                    optarg = nargv[optind++];
                }
            }
            if ((long_options[match].has_arg == required_argument) &&
                (optarg == NULL))
            {
                /*
                 * Missing argument; leading ':'
                 * indicates no error should be generated
                 */
                if (PRINT_ERROR)
                    warnx(recargstring, current_argv);
                if (long_options[match].flag == NULL)
                    optopt = long_options[match].val;
                else
                    optopt = 0;
                --optind;
                return BADARG;
            }
        }
        else
        {
            /* unknown option */
            if (PRINT_ERROR)
                warnx(illoptstring, current_argv);
            optopt = 0;
            return BADCH;
        }
        if (long_options[match].flag)
        {
            *long_options[match].flag = long_options[match].val;
            retval = 0;
        }
        else
        {
            retval = long_options[match].val;
        }
        if (idx)
            *idx = match;
    }
    return retval;
#undef IDENTICAL_INTERPRETATION
}