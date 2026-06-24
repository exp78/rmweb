#include <stdio.h>
#include <sys/utsname.h>

static void cat(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { printf("(cannot read %s)\n", path); return; }
    char buf[256];
    while (fgets(buf, sizeof buf, f)) fputs(buf, stdout);
    fclose(f);
}

int main(void) {
    struct utsname u;
    uname(&u);
    printf("rmweb hello — running on the reMarkable Paper Pro\n");
    printf("  kernel : %s %s\n", u.sysname, u.release);
    printf("  arch   : %s\n", u.machine);
    printf("  soc    : "); cat("/sys/devices/soc0/machine");
    return 0;
}
