/*
** Copyright 2010, Adam Shanks (@ChainsDD)
** Copyright 2008, Zinx Verituse (@zinxv)
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

#define LOG_TAG "su"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <errno.h>
#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <stdint.h>
#include <pwd.h>

#include <private/android_filesystem_config.h>
#include <cutils/log.h>
#include <cutils/properties.h>

#include "su.h"

/* Still lazt, will fix this */
static char socket_path[PATH_MAX];

static struct su_initiator su_from = {
    .pid = -1,
    .uid = 0,
    .bin = "",
    .args = "",
};

static struct su_request su_to = {
    .uid = AID_ROOT,
    .command = DEFAULT_COMMAND,
};

static int from_init(struct su_initiator *from)
{
    char path[PATH_MAX], exe[PATH_MAX];
    char args[4096], *argv0, *argv_rest;
    int fd;
    ssize_t len;
    int i;
    int err;

    from->uid = getuid();
    from->pid = getppid();

    /* Get the command line */
    snprintf(path, sizeof(path), "/proc/%u/cmdline", from->pid);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        PLOGE("Opening command line");
        return -1;
    }
    len = read(fd, args, sizeof(args));
    err = errno;
    close(fd);
    if (len < 0 || len == sizeof(args)) {
        PLOGEV("Reading command line", err);
        return -1;
    }

    argv0 = args;
    argv_rest = NULL;
    for (i = 0; i < len; i++) {
        if (args[i] == '\0') {
            if (!argv_rest) {
                argv_rest = &args[i+1];
            } else {
                args[i] = ' ';
            }
        }
    }
    args[len] = '\0';

    if (argv_rest) {
        strncpy(from->args, argv_rest, sizeof(from->args));
        from->args[sizeof(from->args)-1] = '\0';
    } else {
        from->args[0] = '\0';
    }

    /* If this isn't app_process, use the real path instead of argv[0] */
    snprintf(path, sizeof(path), "/proc/%u/exe", from->pid);
    len = readlink(path, exe, sizeof(exe));
    if (len < 0) {
        PLOGE("Getting exe path");
        return -1;
    }
    exe[len] = '\0';
    if (strcmp(exe, "/system/bin/app_process")) {
        argv0 = exe;
    }

    strncpy(from->bin, argv0, sizeof(from->bin));
    from->bin[sizeof(from->bin)-1] = '\0';

    return 0;
}

static void socket_cleanup(void)
{
    unlink(socket_path);
}

static void cleanup(void)
{
    socket_cleanup();
}

static void cleanup_signal(int sig)
{
    socket_cleanup();
    exit(sig);
}

static int socket_create_temp(char *path, size_t len)
{
    int fd;
    struct sockaddr_un sun;

    fd = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (fd < 0) {
        PLOGE("socket");
        return -1;
    }

    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_LOCAL;
    snprintf(path, len, "%s/.socket%d", REQUESTOR_CACHE_PATH, getpid());
    snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", path);

    /*
     * Delete the socket to protect from situations when
     * something bad occured previously and the kernel reused pid from that process.
     * Small probability, isn't it.
     */
    unlink(sun.sun_path);

    if (bind(fd, (struct sockaddr*)&sun, sizeof(sun)) < 0) {
        PLOGE("bind");
        goto err;
    }

    if (listen(fd, 1) < 0) {
        PLOGE("listen");
        goto err;
    }

    return fd;
err:
    close(fd);
    return -1;
}

static int socket_accept(int serv_fd)
{
    struct timeval tv;
    fd_set fds;
    int fd;

    /* Wait 20 seconds for a connection, then give up. */
    tv.tv_sec = 20;
    tv.tv_usec = 0;
    FD_ZERO(&fds);
    FD_SET(serv_fd, &fds);
    if (select(serv_fd + 1, &fds, NULL, NULL, &tv) < 1) {
        PLOGE("select");
        return -1;
    }

    fd = accept(serv_fd, NULL, NULL);
    if (fd < 0) {
        PLOGE("accept");
        return -1;
    }

    return fd;
}

static int socket_send_request(int fd, struct su_initiator *from, struct su_request *to)
{
    size_t len;
    size_t bin_size, cmd_size;

#define write_token(fd, data)				\
do {							\
	uint32_t __data = htonl(data);			\
	size_t __count = sizeof(__data);		\
	size_t __len = write((fd), &__data, __count);	\
	if (__len != __count) {				\
		PLOGE("write(" #data ")");		\
		return -1;				\
	}						\
} while (0)

    write_token(fd, PROTO_VERSION);
    write_token(fd, PATH_MAX);
    write_token(fd, ARG_MAX);
    write_token(fd, from->uid);
    write_token(fd, to->uid);
    bin_size = strlen(from->bin) + 1;
    write_token(fd, bin_size);
    len = write(fd, from->bin, bin_size);
    if (len != bin_size) {
        PLOGE("write(bin)");
        return -1;
    }
    cmd_size = strlen(to->command) + 1;
    write_token(fd, cmd_size);
    len = write(fd, to->command, cmd_size);
    if (len != cmd_size) {
        PLOGE("write(cmd)");
        return -1;
    }
    return 0;
}

static int socket_receive_result(int fd, char *result, ssize_t result_len)
{
    ssize_t len;
    
    len = read(fd, result, result_len-1);
    if (len < 0) {
        PLOGE("read(result)");
        return -1;
    }
    result[len] = '\0';

    return 0;
}

static void usage(int status)
{
    FILE *stream = (status == EXIT_SUCCESS) ? stdout : stderr;

    fprintf(stream,
    "Usage: su [options] [LOGIN]\n\n"
    "Options:\n"
    "  -c, --command COMMAND         pass COMMAND to the invoked shell\n"
    "  -h, --help                    display this help message and exit\n"
    "  -, -l, --login, -m, -p,\n"
    "  --preserve-environment        do nothing, kept for compatibility\n"
    "  -s, --shell SHELL             use SHELL instead of the default " DEFAULT_COMMAND "\n"
    "  -v, --version                 display version number and exit\n"
    "  -V                            display version code and exit,\n"
    "                                this is used almost exclusively by Superuser.apk\n");
    exit(status);
}

static void deny(void)
{
    struct su_initiator *from = &su_from;
    struct su_request *to = &su_to;

    send_intent(&su_from, &su_to, "", 0, 1);
    LOGW("request rejected (%u->%u %s)", from->uid, to->uid, to->command);
    fprintf(stderr, "%s\n", strerror(EACCES));
    exit(EXIT_FAILURE);
}

static void allow(char *shell, mode_t mask)
{
    struct su_initiator *from = &su_from;
    struct su_request *to = &su_to;
    char *exe = NULL;
    int err;

    umask(mask);
    send_intent(&su_from, &su_to, "", 1, 1);

    if (!shell) {
        shell = DEFAULT_COMMAND;
    }
    exe = strrchr (shell, '/');
    exe = (exe) ? exe + 1 : shell;
    setresgid(to->uid, to->uid, to->uid);
    setresuid(to->uid, to->uid, to->uid);
    LOGD("%u %s executing %u %s using shell %s : %s", from->uid, from->bin,
            to->uid, to->command, shell, exe);
    if (strcmp(to->command, DEFAULT_COMMAND)) {
        execl(shell, exe, "-c", to->command, (char*)NULL);
    } else {
        execl(shell, exe, "-", (char*)NULL);
    }
    err = errno;
    PLOGE("exec");
    fprintf(stderr, "Cannot execute %s: %s\n", shell, strerror(err));
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
    struct stat st;
    int socket_serv_fd, fd;
    char buf[64], *shell = NULL, *result, debuggable[PROPERTY_VALUE_MAX];
    char enabled[PROPERTY_VALUE_MAX], build_type[PROPERTY_VALUE_MAX];
    int c, dballow;
    mode_t orig_umask;
    struct option long_opts[] = {
        { "command",			required_argument,	NULL, 'c' },
        { "help",			no_argument,		NULL, 'h' },
        { "login",			no_argument,		NULL, 'l' },
        { "preserve-environment",	no_argument,		NULL, 'p' },
        { "shell",			required_argument,	NULL, 's' },
        { "version",			no_argument,		NULL, 'v' },
        { NULL, 0, NULL, 0 },
    };

    while ((c = getopt_long(argc, argv, "c:hlmps:Vv", long_opts, NULL)) != -1) {
        switch(c) {
        case 'c':
            su_to.command = optarg;
            break;
        case 'h':
            usage(EXIT_SUCCESS);
            break;
        case 'l':    /* for compatibility */
        case 'm':
        case 'p':
            break;
        case 's':
            shell = optarg;
            break;
        case 'V':
            printf("%d\n", VERSION_CODE);
            exit(EXIT_SUCCESS);
        case 'v':
            printf("%s\n", VERSION);
            exit(EXIT_SUCCESS);
        default:
            /* Bionic getopt_long doesn't terminate its error output by newline */
            fprintf(stderr, "\n");
            usage(2);
        }
    }
    if (optind < argc && !strcmp(argv[optind], "-")) {
        optind++;
    }
    /*
     * Other su implementations pass the remaining args to the shell.
     * -- maybe implement this later
     */
    if (optind < argc - 1) {
        usage(2);
    }
    if (optind == argc - 1) {
        struct passwd *pw;
        pw = getpwnam(argv[optind]);
        if (!pw) {
            char *endptr;

            /* It seems we shouldn't do this at all */
            errno = 0;
            su_to.uid = strtoul(argv[optind], &endptr, 10);
            if (errno || *endptr) {
                LOGE("Unknown id: %s\n", argv[optind]);
                fprintf(stderr, "Unknown id: %s\n", argv[optind]);
                exit(EXIT_FAILURE);
            }
        } else {
            su_to.uid = pw->pw_uid;
        }
    }

    if (from_init(&su_from) < 0) {
        deny();
    }

    property_get("ro.debuggable", debuggable, "0");
    property_get("persist.sys.root_access", enabled, "1");
    property_get("ro.build.type", build_type, "");

    orig_umask = umask(027);

    // only allow su on debuggable builds
    if (strcmp("1", debuggable) != 0) {
        LOGE("Root access is disabled on non-debug builds");
        deny();
    }

    // enforce persist.sys.root_access on non-eng builds
    if (strcmp("eng", build_type) != 0 &&
            (atoi(enabled) & 1) != 1 ) {
        LOGE("Root access is disabled by system setting - enable it under settings -> developer options");
        deny();
    }

    // disallow su in a shell if appropriate
    if (su_from.uid == AID_SHELL && (atoi(enabled) == 1)) {
        LOGE("Root access is disabled by a system setting - enable it under settings -> developer options");
        deny();
    }

    if (su_from.uid == AID_ROOT || su_from.uid == AID_SHELL)
        allow(shell, orig_umask);

    if (stat(REQUESTOR_DATA_PATH, &st) < 0) {
        PLOGE("stat");
        deny();
    }

    if (st.st_gid != st.st_uid)
    {
        LOGE("Bad uid/gid %d/%d for Superuser Requestor application",
                (int)st.st_uid, (int)st.st_gid);
        deny();
    }

    mkdir(REQUESTOR_CACHE_PATH, 0770);
    chown(REQUESTOR_CACHE_PATH, st.st_uid, st.st_gid);

    setgroups(0, NULL);
    setegid(st.st_gid);
    seteuid(st.st_uid);

    dballow = database_check(&su_from, &su_to);
    switch (dballow) {
        case DB_DENY: deny();
        case DB_ALLOW: allow(shell, orig_umask);
        case DB_INTERACTIVE: break;
        default: deny();
    }
    
    socket_serv_fd = socket_create_temp(socket_path, sizeof(socket_path));
    if (socket_serv_fd < 0) {
        deny();
    }

    signal(SIGHUP, cleanup_signal);
    signal(SIGPIPE, cleanup_signal);
    signal(SIGTERM, cleanup_signal);
    signal(SIGABRT, cleanup_signal);
    atexit(cleanup);

    if (send_intent(&su_from, &su_to, socket_path, -1, 0) < 0) {
        deny();
    }

    fd = socket_accept(socket_serv_fd);
    if (fd < 0) {
        deny();
    }
    if (socket_send_request(fd, &su_from, &su_to)) {
        deny();
    }
    if (socket_receive_result(fd, buf, sizeof(buf))) {
        deny();
    }

    close(fd);
    close(socket_serv_fd);
    socket_cleanup();

    result = buf;

#define SOCKET_RESPONSE	"socket:"
    if (strncmp(result, SOCKET_RESPONSE, sizeof(SOCKET_RESPONSE) - 1))
        LOGW("SECURITY RISK: Requestor still receives credentials in intent");
    else
        result += sizeof(SOCKET_RESPONSE) - 1;

    if (!strcmp(result, "DENY")) {
        deny();
    } else if (!strcmp(result, "ALLOW")) {
        allow(shell, orig_umask);
    } else {
        LOGE("unknown response from Superuser Requestor: %s", result);
        deny();
    }

    deny();
    return -1;
}
