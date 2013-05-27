/*
** Copyright 2006, The Android Open Source Project
**           2013 Simon Busch <morphis@gravedo.de>
**           2012 Carsten Munk <carsten.munk@gmail.com>
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <linux/capability.h>
#include <linux/prctl.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#define ANDROID_SOCKET_ENV_PREFIX       "ANDROID_SOCKET_"
#define ANDROID_SOCKET_DIR              "/dev/socket"

#define MAX_LIB_ARGS        16

static const char *ENV[32];

static int make_argv(char * args, char ** argv)
{
    // Note: reserve argv[0]
    int count = 1;
    char * tok;
    char * s = args;

    while ((tok = strtok(s, " \0"))) {
        argv[count] = tok;
        s = NULL;
        count++;
    }
    return count;
}

static char *get_property_value(const char *propfile, const char *key)
{
    FILE *f = fopen(propfile, "r");
    char buf[1024];
    char *mkey, *value;

    if (!f)
        return NULL;

    while (fgets(buf, 1024, f) != NULL) {
        if (strchr(buf, '\r'))
            *(strchr(buf, '\r')) = '\0';
        if (strchr(buf, '\n'))
            *(strchr(buf, '\n')) = '\0';

        mkey = strtok(buf, "=");

        if (!mkey)
            continue;

        value = strtok(NULL, "=");
        if (!value)
            continue;

        if (strcmp(key, mkey) == 0) {
            fclose(f);
            return strdup(value);
        }
    }

    fclose(f);
    return NULL;
}

/*
 * create_socket - creates a Unix domain socket in ANDROID_SOCKET_DIR
 * ("/dev/socket") as dictated in init.rc. This socket is inherited by the
 * daemon. We communicate the file descriptor's value via the environment
 * variable ANDROID_SOCKET_ENV_PREFIX<name> ("ANDROID_SOCKET_foo").
 */
int create_socket(const char *name, int type, mode_t perm, uid_t uid, gid_t gid)
{
    struct sockaddr_un addr;
    int fd, ret;

    fd = socket(PF_UNIX, type, 0);
    if (fd < 0) {
        fprintf(stderr, "Failed to open socket '%s': %s\n", name, strerror(errno));
        return -1;
    }

    memset(&addr, 0 , sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), ANDROID_SOCKET_DIR"/%s",
             name);

    ret = unlink(addr.sun_path);
    if (ret != 0 && errno != ENOENT) {
        fprintf(stderr, "Failed to unlink old socket '%s': %s\n", name, strerror(errno));
        goto out_close;
    }

    ret = bind(fd, (struct sockaddr *) &addr, sizeof (addr));
    if (ret) {
        fprintf(stderr, "Failed to bind socket '%s': %s\n", name, strerror(errno));
        goto out_unlink;
    }

    chown(addr.sun_path, uid, gid);
    chmod(addr.sun_path, perm);

    printf("Created socket '%s' with mode '%o', user '%d', group '%d'\n",
         addr.sun_path, perm, uid, gid);

    return fd;

out_unlink:
    unlink(addr.sun_path);
out_close:
    close(fd);
    return -1;
}

void publish_socket(const char *name, int fd)
{
    char key[64] = ANDROID_SOCKET_ENV_PREFIX;
    char val[64];
    int n;

    strncpy(key + sizeof(ANDROID_SOCKET_ENV_PREFIX) - 1,
            name,
            sizeof(key) - sizeof(ANDROID_SOCKET_ENV_PREFIX));
    snprintf(val, sizeof(val), "%d", fd);

    for (n = 0; n < 31; n++) {
        if (!ENV[n]) {
            ssize_t len = strlen(key) + strlen(val) + 2;
            char *entry = malloc(len);
            snprintf(entry, len, "%s=%s", key, val);
            ENV[n] = entry;
            break;
        }
    }

    /* make sure we don't close-on-exec */
    fcntl(fd, F_SETFD, 0);
}

int main(int argc, char **argv)
{
    int fd;
    char *rilImplLib;
    char *rilArgs;
    char *newArgv[MAX_LIB_ARGS];
    char *s, *tok;
    int count = 4;

    rilImplLib = get_property_value("/system/build.prop", "rild.libpath");
    if (!rilImplLib) {
        fprintf(stderr, "[ERROR] No ril implemenatation specified!");
        exit(1);
    }

    rilArgs = get_property_value("/system/build.prop", "rild.libargs");

    newArgv[0] = "/system/bin/rild";
    newArgv[1] = "-l";
    newArgv[2] = rilImplLib;
    newArgv[3] = "--";

    s = rilArgs;
    while ((tok = strtok(s, " \0"))) {
        newArgv[count] = tok;
        s = NULL;
        count++;
    }

    newArgv[count] = '\0';

    fd = create_socket("rild", SOCK_STREAM, 0660, 0, 0);
    publish_socket("rild", fd);

    fd = create_socket("rild-debug", SOCK_STREAM, 0660, 0, 0);
    publish_socket("rild-debug", fd);

    int err = execvpe("/system/bin/rild", newArgv, ENV);
    if (err < 0) {
        perror("Failed to launch rild");
        exit(1);
    }

    return 0;
}

