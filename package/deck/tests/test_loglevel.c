// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * test_loglevel.c - the log level grammar (shared/loglevel.h).
 *
 * Both binaries parse their level from the environment with this function, so
 * what it accepts is the interface for EP122_MOD_LOGLEVEL and STEMD_LOGLEVEL.
 * Most cases cover slightly wrong input.
 */
#include "loglevel.h"

#include "test.h"

int main(void)
{
    T_CASE("names");
    CHECK_INT(log_level_from("error"), LOG_ERROR);
    CHECK_INT(log_level_from("warn"),  LOG_WARN);
    CHECK_INT(log_level_from("info"),  LOG_INFO);
    CHECK_INT(log_level_from("debug"), LOG_DEBUG);
    CHECK_INT(log_level_from("trace"), LOG_TRACE);

    T_CASE("case-insensitive");
    CHECK_INT(log_level_from("ERROR"), LOG_ERROR);
    CHECK_INT(log_level_from("Debug"), LOG_DEBUG);
    CHECK_INT(log_level_from("TrAcE"), LOG_TRACE);

    T_CASE("digits");
    CHECK_INT(log_level_from("0"), LOG_ERROR);
    CHECK_INT(log_level_from("3"), LOG_DEBUG);
    CHECK_INT(log_level_from("4"), LOG_TRACE);
    /* A digit above the top level clamps to trace. */
    CHECK_INT(log_level_from("9"), LOG_TRACE);

    T_CASE("unset is the default, not an error");
    CHECK_INT(log_level_from(NULL), LOG_ERROR);
    CHECK_INT(log_level_from(""),   LOG_ERROR);
    CHECK_INT(log_level_from("  "), LOG_ERROR);

    T_CASE("leading blanks");
    CHECK_INT(log_level_from(" debug"),   LOG_DEBUG);
    CHECK_INT(log_level_from("\tinfo"),   LOG_INFO);
    CHECK_INT(log_level_from("  warn  "), LOG_WARN);

    /* -1 lets the caller report a bad value instead of silently falling back
     * to the quietest level. */
    T_CASE("unrecognised is -1, never a level");
    CHECK_INT(log_level_from("verbose"), -1);
    CHECK_INT(log_level_from("quiet"),   -1);
    CHECK_INT(log_level_from("on"),      -1);
    CHECK_INT(log_level_from("1=true"),  -1);
    CHECK_INT(log_level_from("-1"),      -1);
    CHECK_INT(log_level_from("10"),      -1);   /* two digits is not a level */

    T_CASE("whole word only");
    CHECK_INT(log_level_from("infomercial"), -1);
    CHECK_INT(log_level_from("debugger"),    -1);
    CHECK_INT(log_level_from("warning"),     -1);
    CHECK_INT(log_level_from("traceroute"),  -1);
    CHECK_INT(log_level_from("err"),         -1);   /* a prefix is not the name */

    return t_done("loglevel");
}
