/**
 * test_qm_fatal.c — host test of QmFatalText, the match for qm's no-dialog failure line.
 *
 * Exit 0 when every case passes, 1 otherwise.
 */
#include "qm_fatal.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int cond, const char *what) {
    printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) failures++;
}

int main(void) {
    const char *line = "[qm] FATAL-NODIALOG act-mod at world.begin: boom";
    const char *t = QmFatalText(line);
    check(t != NULL && strcmp(t, "act-mod at world.begin: boom") == 0, "the failure line gives its text");
    check(QmFatalText("[qm] FATAL-NODIALOG act-mod at world.begin: boom  @qm_modloader:12") != NULL,
          "a line with the caller's location appended still matches");
    check(QmFatalText("[qm] FATAL act-mod at world.begin: boom") == NULL,
          "the FATAL line every failure prints first is not the no-dialog line");
    check(QmFatalText("[qm] FATAL-NODIALOG ") == NULL, "the prefix alone is not a failure");
    check(QmFatalText("[qm] FATAL-NODIALOG") == NULL, "the prefix without its space is not one");
    check(QmFatalText(" [qm] FATAL-NODIALOG x") == NULL, "the prefix must start the line");
    check(QmFatalText("[QM] FATAL-NODIALOG x") == NULL, "the match is case-sensitive, as qm prints it");
    check(QmFatalText("") == NULL, "an empty line is not one");
    check(QmFatalText(NULL) == NULL, "no line is not one");
    return failures ? 1 : 0;
}
