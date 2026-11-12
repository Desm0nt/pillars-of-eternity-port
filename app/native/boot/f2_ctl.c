/* F2 control file: lets the main session press pad buttons over adb while there is no on-screen pad yet.
 *   adb shell "echo 'pad A' >> /sdcard/Android/data/<package>/files/f2_ctl.txt"
 * Lines APPENDED to the file (">>") are run as they arrive; a file rewritten with ">" (shorter than before) is run
 * from its start.  The file is never deleted by the app.  One command per line, executed in order:
 *   pad <BTN[+BTN..]> [ms]        press and hold (default 150 ms), then release.  Buttons: A B X Y L R ZL ZR PLUS MINUS
 *                                 UP DOWN LEFT RIGHT LS RS
 *   stick <lx> <ly> [rx ry] [ms]  hold the sticks (-32767..32767) for ms (default 300), then centre
 *   wait <ms>
 *   mark <text>                   a line in the log
 * The pad is the one handheld controller of native/nn/f2_nn_hid.c (f2_nn_pad_set). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include "f2_boot.h"

void f2_nn_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
void f2_touch_set_mode(int touch);   /* a pad command is pad input: the game goes to its console input, as with the on-screen pad */

static const struct { const char *name; uint64_t bit; } k_btn[] = {
    { "A", 1 << 0 }, { "B", 1 << 1 }, { "X", 1 << 2 }, { "Y", 1 << 3 }, { "LS", 1 << 4 }, { "RS", 1 << 5 }, { "L", 1 << 6 }, { "R", 1 << 7 },
    { "ZL", 1 << 8 }, { "ZR", 1 << 9 }, { "PLUS", 1 << 10 }, { "MINUS", 1 << 11 }, { "LEFT", 1 << 12 }, { "UP", 1 << 13 }, { "RIGHT", 1 << 14 },
    { "DOWN", 1 << 15 },
};

static void run_line(char *line)
{
    char *save = NULL, *cmd = strtok_r(line, " \t\r\n", &save);
    if (!cmd || cmd[0] == '#') return;
    if (!strcasecmp(cmd, "pad") || !strcasecmp(cmd, "stick")) f2_touch_set_mode(0);
    if (!strcasecmp(cmd, "pad")) {
        char *names = strtok_r(NULL, " \t\r\n", &save), *ms = strtok_r(NULL, " \t\r\n", &save);
        uint64_t bits = 0;
        char *s2 = NULL;
        for (char *n = names ? strtok_r(names, "+", &s2) : NULL; n; n = strtok_r(NULL, "+", &s2)) {
            int found = 0;
            for (unsigned i = 0; i < sizeof k_btn / sizeof k_btn[0]; i++)
                if (!strcasecmp(n, k_btn[i].name)) { bits |= k_btn[i].bit; found = 1; }
            if (!found) f2_log("F2: ctl: unknown button '%s'", n);
        }
        int hold = ms ? atoi(ms) : 150;
        f2_log("F2: ctl: pad buttons 0x%llx for %d ms", (unsigned long long)bits, hold);
        f2_nn_pad_set(bits, 0, 0, 0, 0);
        usleep(hold * 1000);
        f2_nn_pad_set(0, 0, 0, 0, 0);
        usleep(80 * 1000);
    } else if (!strcasecmp(cmd, "stick")) {
        int v[5] = { 0, 0, 0, 0, 300 }, n = 0;
        for (char *t; n < 5 && (t = strtok_r(NULL, " \t\r\n", &save));) v[n++] = atoi(t);
        if (n == 3) { v[4] = v[2]; v[2] = 0; }
        f2_log("F2: ctl: sticks L(%d,%d) R(%d,%d) for %d ms", v[0], v[1], v[2], v[3], v[4]);
        f2_nn_pad_set(0, v[0], v[1], v[2], v[3]);
        usleep(v[4] * 1000);
        f2_nn_pad_set(0, 0, 0, 0, 0);
    } else if (!strcasecmp(cmd, "wait")) {
        char *t = strtok_r(NULL, " \t\r\n", &save);
        usleep((t ? atoi(t) : 100) * 1000);
    } else if (!strcasecmp(cmd, "mark")) {
        f2_log("F2: ctl: mark %s", save ? save : "");
    } else f2_log("F2: ctl: unknown command '%s'", cmd);
}

static void *ctl_thread(void *arg)
{
    (void)arg;
    char path[700];
    snprintf(path, sizeof path, "%s/f2_ctl.txt", f2_cfg.files_dir);
    struct stat st;
    long off = 0;
    if (stat(path, &st) == 0) { off = (long)st.st_size; f2_log("F2: ctl: %s has %ld old bytes (skipped); new lines appended to it are run", path, off); }
    for (;;) {
        usleep(100 * 1000);
        if (stat(path, &st) != 0) { off = 0; continue; }
        if ((long)st.st_size == off) continue;
        if ((long)st.st_size < off) off = 0;                 /* rewritten with ">" : run it from the start */
        FILE *f = fopen(path, "r");
        if (!f) continue;
        fseek(f, off, SEEK_SET);
        char line[256];
        while (fgets(line, sizeof line, f)) run_line(line);
        off = ftell(f);
        fclose(f);
    }
    return NULL;
}

void f2_ctl_start(void)
{
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, ctl_thread, NULL) == 0) pthread_setname_np(th, "f2-ctl");
    pthread_attr_destroy(&at);
}
