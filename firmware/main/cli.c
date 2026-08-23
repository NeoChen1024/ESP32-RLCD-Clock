#include "cli.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_console.h"
#include "linenoise/linenoise.h"
#include "model.h"
#include "sensors.h"
#include "sntp_mgr.h"
#include "wifi_mgr.h"
static const char *state_str(wifi_mgr_state_t s)
{
    switch (s) {
    case WIFI_MGR_DISCONNECTED: return "disconnected";
    case WIFI_MGR_CONNECTING:   return "connecting";
    case WIFI_MGR_CONNECTED:    return "connected";
    }
    return "?";
}

/* ---- wifi <connect|status|disconnect> ... ---- */
static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: wifi connect <ssid> [password] | wifi status | wifi disconnect\n");
        return 1;
    }
    if (strcmp(argv[1], "connect") == 0) {
        if (argc < 3) {
            printf("usage: wifi connect <ssid> [password]\n");
            return 1;
        }
        const char *pass = argc >= 4 ? argv[3] : "";
        printf("connecting to \"%s\" ...\n", argv[2]);
        if (!wifi_mgr_connect(argv[2], pass, 0)) {
            printf("failed to start connect\n");
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[1], "status") == 0) {
        wifi_mgr_status_t st = wifi_mgr_status();
        printf("state: %s\n", state_str(st.state));
        printf("ssid:  %s\n", st.ssid[0] ? st.ssid : "-");
        printf("ip:    %s\n", st.ip[0] ? st.ip : "-");
        if (st.rssi_dbm != 0) printf("rssi:  %d dBm\n", st.rssi_dbm);
        return 0;
    }
    if (strcmp(argv[1], "disconnect") == 0) {
        wifi_mgr_disconnect();
        printf("disconnected\n");
        return 0;
    }
    printf("unknown wifi subcommand: %s\n", argv[1]);
    return 1;
}

/* ---- ntp <status|server|reset|resync> ---- */
static int cmd_ntp(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: ntp status | ntp server <host|ip> | ntp reset | ntp resync\n");
        return 1;
    }
    if (strcmp(argv[1], "status") == 0) {
        sntp_mgr_status_t st = sntp_mgr_status();
        printf("started: %s\n", st.started ? "yes" : "no");
        printf("synced:  %s\n", st.synced ? "yes" : "no");
        printf("source:  %s\n", st.using_manual ? "manual" :
                                 st.using_dhcp ? "dhcp" : "fallback");
        printf("server_name: %s\n", st.server0);
        printf("server_ip:   %s\n", st.server0_ip);
        if (st.started) {
            time_t t = (time_t)st.unix_sec;
            struct tm tm;
            gmtime_r(&t, &tm);
            char buf[32];
            strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S UTC", &tm);
            printf("time:    %s\n", buf);
        }
        return 0;
    }
    if (strcmp(argv[1], "server") == 0) {
        if (argc < 3) {
            printf("usage: ntp server <host|ip>\n");
            return 1;
        }
        if (!sntp_mgr_set_server(argv[2])) {
            printf("invalid server\n");
            return 1;
        }
        printf("ntp server set to %s\n", argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "reset") == 0) {
        sntp_mgr_reset_servers();
        printf("ntp servers reset (dhcp/fallback)\n");
        return 0;
    }
    if (strcmp(argv[1], "resync") == 0) {
        sntp_mgr_resync();
        printf("resync requested\n");
        return 0;
    }
    printf("unknown ntp subcommand: %s\n", argv[1]);
    return 1;
}

/* ---- http <status> ---- */
static int cmd_http(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("endpoints:\n");
    printf("  /             HTML preview\n");
    printf("  /status       JSON state\n");
    printf("  /snapshot.pbm P4 PBM framebuffer\n");
    printf("  /snapshot.bmp 1-bit BMP framebuffer\n");
    return 0;
}

/* ---- tz <±HH:MM | ±HHMM | reset> ---- */
static int cmd_tz(int argc, char **argv)
{
    if (argc < 2) {
        int tz = model_tz_get();
        printf("tz: %c%02d:%02d (%+d min)\n",
               tz < 0 ? '-' : '+', (tz < 0 ? -tz : tz) / 60,
               (tz < 0 ? -tz : tz) % 60, tz);
        printf("usage: tz <offset> | tz reset\n");
        printf("  offset: +HH:MM, -HH:MM, +HHMM, or bare minutes (e.g. 480)\n");
        return 0;
    }
    if (strcmp(argv[1], "reset") == 0) {
        model_tz_set_default();
        printf("tz reset to default (UTC+8)\n");
        return 0;
    }
    const char *p = argv[1];
    int sign = 1;
    if (*p == '+' || *p == '-') {
        if (*p == '-') sign = -1;
        p++;
    }
    int minutes = 0;
    if (strchr(p, ':')) {
        /* HH:MM */
        int h = 0, mi = 0;
        if (sscanf(p, "%d:%d", &h, &mi) != 2 || h < 0 || h > 23 || mi < 0 || mi > 59) {
            printf("invalid offset\n");
            return 1;
        }
        minutes = h * 60 + mi;
    } else {
        /* HHMM (4 digits) or bare minutes */
        size_t len = strlen(p);
        if (len == 4 && p[0] >= '0' && p[0] <= '2') {
            int h = (p[0] - '0') * 10 + (p[1] - '0');
            int mi = (p[2] - '0') * 10 + (p[3] - '0');
            if (h > 23 || mi > 59) {
                printf("invalid offset\n");
                return 1;
            }
            minutes = h * 60 + mi;
        } else {
            char *end;
            long v = strtol(p, &end, 10);
            if (*p == '\0' || *end != '\0' || v < -840 || v > 840) {
                printf("invalid offset\n");
                return 1;
            }
            minutes = (int)v;
        }
    }
    model_tz_set(sign * minutes);
    return 0;
}

/* ---- sensor read ---- */
static int cmd_sensor(int argc, char **argv)
{
    (void)argc; (void)argv;
    float t = 0, rh = 0;
    if (sensors_read_temp_humi(&t, &rh)) {
        printf("temp: +%.1f C\n", t);
        printf("rh:   %.0f %%\n", rh);
    } else {
        printf("temp: n/a (SHTC3 not responding)\n");
    }
    printf("batt: %.2f V\n", sensors_read_batt_v());
    return 0;
}

static void register_cmds(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "wifi", .help = "Wi-Fi STA control: connect <ssid> [password] | status | disconnect",
          .hint = "connect \"<ssid>\" [password] | status | disconnect",
          .func = cmd_wifi },
        { .command = "ntp", .help = "SNTP control: status | server <host|ip> | reset | resync",
          .hint = "status | server \"<host|ip>\" | reset | resync",
          .func = cmd_ntp },
        { .command = "http", .help = "show HTTP debug endpoints",
          .func = cmd_http },
        { .command = "tz", .help = "show/set local timezone offset",
          .hint = "[+HH:MM | -HH:MM | +HHMM | minutes] | reset",
          .func = cmd_tz },
        { .command = "sensor", .help = "read SHTC3 temp/humidity and battery voltage",
          .func = cmd_sensor },
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        esp_console_cmd_register(&cmds[i]);
    }
}

static void init_console_peripheral(void)
{
    /* USB-Serial/JTAG console (the board's only host-facing serial port).
     * Matches CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y. */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    fcntl(fileno(stdout), F_SETFL, 0);
    fcntl(fileno(stdin), F_SETFL, 0);

    usb_serial_jtag_driver_config_t jtag = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&jtag));
    usb_serial_jtag_vfs_use_driver();
}

void cli_start(void)
{
    init_console_peripheral();

    esp_console_config_t console_cfg = {
        .max_cmdline_length = 256,
        .max_cmdline_args = 16,
        .hint_color = 32,
    };
    ESP_ERROR_CHECK(esp_console_init(&console_cfg));
    esp_console_register_help_command();
    register_cmds();

    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);   /* prompt has no newline; never buffer */

    /* Over a USB-Serial/JTAG console there is no real terminal to negotiate
     * escape sequences with; disable line editing so linenoise uses a plain
     * fgets-style read loop (which works over the serial VFS). */
    linenoiseSetDumbMode(1);

    printf("\nrlcd bring-up console. type 'help'.\n");

    const char *prompt = "rlcd> ";
    for (;;) {
        char *line = linenoise(prompt);
        if (line == NULL) {   /* EOF / Ctrl-D */
            printf("\n");
            continue;
        }
        if (line[0] != '\0') {
            linenoiseHistoryAdd(line);
            int ret = 0;
            esp_err_t err = esp_console_run(line, &ret);
            if (err == ESP_ERR_NOT_FOUND) {
                printf("unrecognized command\n");
            } else if (err == ESP_ERR_INVALID_ARG) {
                /* no-op */
            }
        }
        linenoiseFree(line);
    }
}
