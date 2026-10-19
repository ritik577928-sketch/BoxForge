/* file_write_test - write a marker file and read it back (overlay test).
 * usage: file_write_test PATH [TEXT] */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/tmp/bf_written";
    const char *text = argc > 2 ? argv[2] : "written-by-container";
    char buf[128] = "";
    FILE *f = fopen(path, "w");

    if (!f || fprintf(f, "%s\n", text) < 0 || fclose(f) != 0) {
        perror("file_write_test: write");
        return 1;
    }
    f = fopen(path, "r");
    if (!f || !fgets(buf, sizeof buf, f)) {
        perror("file_write_test: read");
        return 1;
    }
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
    printf("file_write_test: wrote and read back '%s' at %s\n", buf, path);
    return strcmp(buf, text) ? 1 : 0;
}
