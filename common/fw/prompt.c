/* SquirrelBIOS common/fw: the firmware command prompt.
 * Original code. */
#include "fwlib.h"

#define LINE_MAX 128
#define ARGS_MAX 8

int fw_readline(char *buf, int size)
{
    int n = 0;
    for (;;) {
        int c = fw_getc();
        if (c < 0)
            continue;
        if (c == '\r' || c == '\n') {
            fw_puts("\n");
            buf[n] = 0;
            return n;
        }
        if (c == 8 || c == 127) {
            if (n) {
                n--;
                fw_puts("\b \b");
            }
            continue;
        }
        if (c < 32 || c > 126 || n + 1 >= size)
            continue;
        buf[n++] = (char)c;
        fw_putc((char)c);
    }
}

static int split(char *line, char **argv)
{
    int argc = 0;
    while (*line && argc < ARGS_MAX) {
        while (*line == ' ' || *line == '\t')
            *line++ = 0;
        if (!*line)
            break;
        argv[argc++] = line;
        while (*line && *line != ' ' && *line != '\t')
            line++;
    }
    return argc;
}

void fw_prompt(const struct fw_cmd *cmds)
{
    char line[LINE_MAX];
    char *argv[ARGS_MAX];
    for (;;) {
        fw_puts("sq> ");
        fw_readline(line, sizeof(line));
        int argc = split(line, argv);
        if (!argc)
            continue;
        if (!strcmp(argv[0], "help")) {
            fw_puts("commands:\n");
            for (const struct fw_cmd *c = cmds; c->name; c++)
                fw_printf("  %-10s %s\n", c->name, c->help);
            fw_printf("  %-10s %s\n", "help", "this list");
            continue;
        }
        const struct fw_cmd *c;
        for (c = cmds; c->name; c++)
            if (!strcmp(c->name, argv[0]))
                break;
        if (c->name)
            c->run(argc, argv);
        else
            fw_printf("unknown command '%s' (try help)\n", argv[0]);
    }
}
