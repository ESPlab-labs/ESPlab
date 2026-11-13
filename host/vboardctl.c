/* vboardctl: drive one virtual board's console from a script.
 * Writes commands to the board's stdin via a FIFO pair. Simpler: we spawn
 * the target vboard ourselves with stdin/stdout plumbed. But vboards are
 * already running detached; instead this tool talks to a vboard started
 * WITH --console by the test script. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

int main(int argc, char **argv)
{
    /* usage: vboardctl <vboard-pid> "cmd1" "cmd2" ...
     * nope - simpler: read fifo path from argv[1] */
    if (argc < 3) { fprintf(stderr, "usage: vboardctl <fifo> <cmd>...\n"); return 1; }
    int fd = open(argv[1], O_WRONLY);
    if (fd < 0) { perror("open fifo"); return 1; }
    for (int i = 2; i < argc; i++) {
        write(fd, argv[i], strlen(argv[i]));
        write(fd, "\n", 1);
        usleep(300000);
    }
    close(fd);
    return 0;
}
