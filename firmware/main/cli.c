#include "cli.h"

#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_console.h"
#include "linenoise/linenoise.h"
#include "model.h"
#include "config_mgr.h"
#include "audio_mgr.h"
#include "leap_mgr.h"
#include "wifi_secrets.h"
#include "rtc_mgr.h"
#include "sensors.h"
#include "storage_mgr.h"
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
        printf("usage: wifi connect <ssid> [password] | status | disconnect | reconnect | reset\n");
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
        printf("mode:  %s\n", st.mode == WIFI_MGR_MODE_AUTO ? "auto (known networks)" :
               st.mode == WIFI_MGR_MODE_MANUAL ? "manual (wifi connect)" : "off (wifi reset to resume)");
        if (storage_lock(1000)) {
            wifi_secrets_info_t secrets;
            wifi_secrets_current_locked(&secrets);
            storage_unlock();
            if (secrets.found) printf("known: %u from %s/%s\n", secrets.count, secrets.volume, STORAGE_WIFI_SECRETS);
            else printf("known: none (no valid %s)\n", STORAGE_WIFI_SECRETS);
        }
        printf("state: %s\n", state_str(st.state));
        printf("ssid:  %s\n", st.ssid[0] ? st.ssid : "-");
        printf("ip:    %s\n", st.ip[0] ? st.ip : "-");
        if (st.rssi_dbm != 0) printf("rssi:  %d dBm\n", st.rssi_dbm);
        return 0;
    }
    if (strcmp(argv[1], "disconnect") == 0) {
        wifi_mgr_disconnect();
        printf("disconnected; automatic connection paused until `wifi reset`\n");
        return 0;
    }
    if (strcmp(argv[1], "reset") == 0) {
        wifi_mgr_reset();
        printf("auto mode: scanning for known networks\n");
        return 0;
    }
    if (strcmp(argv[1], "reconnect") == 0) {
        wifi_mgr_reconnect();
        printf("reconnect requested (if credentials are active)\n");
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
        printf("trusted: %s\n", st.time_trusted ? "yes" : "no");
        printf("fresh:   %s\n", st.fresh ? "yes" : "no");
        printf("age:     %lu s\n", (unsigned long)st.ntp_age_s);
        printf("source:  %s\n", st.using_manual ? "manual" :
                                 st.using_config ? "config" :
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
        printf("ntp servers reset (config/DHCP/fallback priority)\n");
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

static void print_offset(const char *label, int minutes)
{
    int a = minutes < 0 ? -minutes : minutes;
    printf("%s%c%02d:%02d (%+d min)", label, minutes < 0 ? '-' : '+', a / 60, a % 60, minutes);
}

/* ---- tz <POSIX rule | ±HH:MM | ±HHMM | minutes | reset> ---- */
static int cmd_tz(int argc, char **argv)
{
    if (argc < 2) {
        tz_rule_t rule;
        bool cli, dst;
        model_tz_get(&rule, &cli);
        printf("tz rule: %s (%s)\n", rule.text, cli ? "CLI override" : "selected config");
        if (sntp_mgr_status().time_trusted) {
            print_offset("now:     ", tz_rule_offset_minutes(&rule, time(NULL), &dst));
            printf("%s\n", dst ? " daylight" : "");
        }
        printf("usage: tz <rule> | tz <offset> | tz reset\n");
        printf("  rule:   POSIX TZ, e.g. CST-8 or CET-1CEST,M3.5.0,M10.5.0/3\n");
        printf("  offset: +HH:MM, -HH:MM, +HHMM, or bare minutes east (e.g. 480)\n");
        return 0;
    }
    if (argc > 2) { printf("TZ rules contain no spaces\n"); return 1; }
    if (strcmp(argv[1], "reset") == 0) {
        model_tz_set_default();
        printf("tz reset to selected config (or %s)\n", MODEL_TZ_DEFAULT);
        return 0;
    }
    tz_rule_t rule;
    const char *p = argv[1];
    /* A POSIX rule starts with a zone name; an offset starts with a sign or
     * digit and, unlike POSIX, counts east of UTC. */
    if (*p == '<' || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
        if (!tz_rule_parse(p, &rule)) { printf("invalid POSIX TZ rule\n"); return 1; }
        model_tz_set(&rule);
        return 0;
    }
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
    if (!tz_rule_fixed(sign * minutes, &rule)) { printf("offset outside -14:00..+14:00\n"); return 1; }
    model_tz_set(&rule);
    printf("tz rule: %s\n", rule.text);
    return 0;
}

static void print_utc_date(const char *label, int64_t unix_s)
{
    time_t t = (time_t)unix_s;
    struct tm tm;
    gmtime_r(&t, &tm);
    printf("%s%04d-%02d-%02d\n", label, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

/* ---- leap [status | reload] ---- */
static int cmd_leap(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "status") && strcmp(argv[1], "reload"))) {
        printf("usage: leap [status] | reload\n"); return 1;
    }
    if (argc == 2 && !strcmp(argv[1], "reload")) {
        if (!storage_lock(5000)) { printf("storage busy\n"); return 1; }
        leap_mgr_reload_locked();
        storage_unlock();
    }
    model_leap_info_t info;
    model_leap_info(&info);
    sntp_mgr_status_t s = sntp_mgr_status();
    if (info.loaded) {
        printf("table:   %s/%s\n", info.volume, LEAP_MGR_RELATIVE);
        print_utc_date("updated: ", info.updated_unix_s);
        print_utc_date("expires: ", info.expires_unix_s);
        printf("state:   %s\n", !s.time_trusted ? "unknown (time untrusted)" :
               s.unix_sec >= info.expires_unix_s ? "EXPIRED, holding last TAI-UTC" : "current");
    } else {
        printf("table:   none; built-in TAI-UTC %d s\n", TAI_MINUS_UTC_BUILTIN_S);
    }
    int tai_utc = model_tai_minus_utc(s.unix_sec);
    printf("TAI-UTC: %d s\nGPS-UTC: %d s\n", tai_utc, tai_utc - TAI_MINUS_GPS_SECONDS);
    return 0;
}

/* ---- audio play [sd|flash] <file> | stop | volume [0-100] | status ---- */
static int cmd_audio(int argc, char **argv)
{
    const char *op = argc > 1 ? argv[1] : "status";
    if (!strcmp(op, "play") && (argc == 3 || argc == 4)) {
        const char *volume = argc == 4 ? argv[2] : "sd";
        const char *name = argv[argc - 1];
        char relative[STORAGE_REL_MAX];
        /* Accept a bare file name or the managed sounds/ path. */
        snprintf(relative, sizeof relative, "%s%s", strncmp(name, "sounds/", 7) ? "sounds/" : "", name);
        if (!audio_mgr_play(volume, relative)) { printf("cannot play %s/%s\n", volume, relative); return 1; }
        printf("queued %s/%s\n", volume, relative);
        return 0;
    }
    if (!strcmp(op, "stop") && argc == 2) { audio_mgr_stop(); printf("stopped\n"); return 0; }
    if (!strcmp(op, "volume") && argc <= 3) {
        if (argc == 3 && !strcmp(argv[2], "reset")) audio_mgr_reset_volume();
        else if (argc == 3) {
            char *end;
            long v = strtol(argv[2], &end, 10);
            if (!*argv[2] || *end || v < 0 || v > 100) { printf("volume is 0..100\n"); return 1; }
            audio_mgr_set_volume((int)v);
        }
        audio_status_t st;
        audio_mgr_status(&st);
        printf("volume: %d (%s)\n", st.volume, st.volume_override ? "CLI override" : "selected config");
        return 0;
    }
    if (!strcmp(op, "status") && argc <= 2) {
        audio_status_t st;
        audio_mgr_status(&st);
        if (!st.available) { printf("audio: unavailable (codec init failed)\n"); return 1; }
        if (st.state == AUDIO_PLAYING)
            printf("playing: %s/%s\nformat:  %lu Hz, %u ch, 16-bit\nposition: %lu.%03lu / %lu.%03lu s\n",
                   st.volume_name, st.relative, (unsigned long)st.sample_rate, st.channels,
                   (unsigned long)st.position_ms / 1000, (unsigned long)st.position_ms % 1000,
                   (unsigned long)st.duration_ms / 1000, (unsigned long)st.duration_ms % 1000);
        else printf("idle\n");
        printf("volume:  %d (%s)\nunderruns: %lu\n", st.volume,
               st.volume_override ? "CLI override" : "selected config", (unsigned long)st.underruns);
        if (st.last_error[0]) printf("last error: %s\n", st.last_error);
        return 0;
    }
    printf("usage: audio play [sd|flash] <file> | stop | volume [0-100|reset] | status\n");
    return 1;
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
    float batt;
    if (sensors_read_batt_v(&batt)) printf("batt: %.2f V\n", batt);
    else printf("batt: n/a (ADC unavailable)\n");
    return 0;
}

static int cmd_sd(int argc, char **argv)
{
    return storage_mgr_command(argc, argv, stdout);
}

static int cmd_flash(int argc, char **argv)
{
    return storage_flash_command(argc, argv, stdout);
}

static void print_cleanup_entry(const char *relative, void *context)
{
    printf("  %s %s\n", *(bool *)context ? "removed" : "remove ", relative);
}

/* config cleanup [sd|flash] [confirm]: preview, or remove older versions. */
static int config_cleanup(int argc, char **argv)
{
    const char *only = NULL;
    bool apply = false;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "confirm")) apply = true;
        else if (!only && (!strcmp(argv[i], "sd") || !strcmp(argv[i], "flash"))) only = argv[i];
        else { printf("usage: config cleanup [sd|flash] [confirm]\n"); return 1; }
    }
    const char *volumes[] = {"sd", "flash"};
    int status = 0;
    if (!storage_lock(5000)) { printf("storage busy\n"); return 1; }
    for (unsigned i = 0; i < 2; ++i) {
        if (only && strcmp(only, volumes[i])) continue;
        char kept[STORAGE_REL_MAX];
        printf("%s:\n", volumes[i]);
        int n = config_mgr_cleanup_locked(volumes[i], apply, kept, print_cleanup_entry, &apply);
        if (n == CONFIG_CLEANUP_UNMOUNTED) printf("  not mounted\n");
        else if (n == CONFIG_CLEANUP_NO_VALID) printf("  no valid version; nothing removed\n");
        else if (n < 0) { printf("  error; remaining files kept\n"); status = 1; }
        else printf("  keep    %s%s\n", kept, n ? "" : " (nothing older)");
    }
    storage_unlock();
    if (!apply) printf("preview only; add `confirm` to remove\n");
    return status;
}

static int cmd_config(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "cleanup")) return config_cleanup(argc, argv);
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "status") && strcmp(argv[1], "reload"))) {
        printf("usage: config [status] | reload | cleanup [sd|flash] [confirm]\n"); return 1;
    }
    if (argc == 2 && !strcmp(argv[1], "reload")) config_mgr_reload();
    if (!storage_lock(5000)) { printf("storage busy\n"); return 1; }
    config_selection_t st;
    config_mgr_current_locked(&st);
    storage_unlock();
    printf("selected: %s\n", st.found ? st.volume : "none");
    if (st.found) printf("file:     %s\n", st.path);
    tz_rule_t effective;
    model_tz_get(&effective, NULL);
    printf("config TZ: %s\neffective TZ: %s\nconfig NTP: %s\nconfig audio volume: %d\n",
           st.tz.text, effective.text, st.ntp_server[0] ? st.ntp_server : "DHCP/fallback", st.audio_volume);
    return 0;
}

static int cmd_rtc(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "status"))) {
        printf("usage: rtc [status]\n"); return 1;
    }
    rtc_mgr_status_t st = rtc_mgr_status();
    printf("present: %s\noscillator stopped: %s\nmarker valid: %s\ncalendar valid: %s\n",
           st.present ? "yes" : "no", st.oscillator_stopped ? "yes" : "no",
           st.marker_valid ? "yes" : "no", st.calendar_valid ? "yes" : "no");
    printf("checkpoint: %s\nboot eligible: %s\nused at boot: %s\n",
           st.anchor_valid ? "yes" : "no", st.eligible ? "yes" : "no",
           st.boot_used ? "yes" : "no");
    if (st.calendar_valid) {
        time_t t = (time_t)st.utc_sec;
        struct tm tm;
        char value[32];
        gmtime_r(&t, &tm);
        strftime(value, sizeof value, "%Y-%m-%d %H:%M:%S UTC", &tm);
        printf("rtc time: %s\n", value);
    }
    if (st.anchor_valid) printf("last SNTP checkpoint: %lld UTC\n", (long long)st.last_sync_sec);
    return st.present ? 0 : 1;
}

static void register_cmds(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "wifi", .help = "Wi-Fi STA control: connect <ssid> [password] | status | disconnect | reconnect | reset",
          .hint = "connect \"<ssid>\" [password] | status | disconnect | reconnect | reset",
          .func = cmd_wifi },
        { .command = "ntp", .help = "SNTP control: status | server <host|ip> | reset | resync",
          .hint = "status | server \"<host|ip>\" | reset | resync",
          .func = cmd_ntp },
        { .command = "http", .help = "show HTTP debug endpoints",
          .func = cmd_http },
        { .command = "tz", .help = "show/set local time zone: <POSIX rule> | <offset> | reset",
          .hint = "[<rule> | +HH:MM | -HH:MM | +HHMM | minutes] | reset",
          .func = cmd_tz },
        { .command = "leap", .help = "TAI-UTC table from time/leap-seconds.list: status | reload",
          .hint = "[status] | reload",
          .func = cmd_leap },
        { .command = "audio", .help = "WAV playback: play [sd|flash] <sounds file> | stop | volume [0-100|reset] | status",
          .hint = "play [sd|flash] \"<file>\" | stop | volume [0-100|reset] | status",
          .func = cmd_audio },
        { .command = "sensor", .help = "read SHTC3 temp/humidity and battery voltage",
          .func = cmd_sensor },
        { .command = "config", .help = "selected version: status | reload | cleanup [sd|flash] [confirm]",
          .func = cmd_config },
        { .command = "rtc", .help = "PCF85063A boot-holdover diagnostics: status",
          .func = cmd_rtc },
        { .command = "sd", .help = "SD card: status | mount | unmount | ls [dir] | cat <file> | test | format (ERASE SD)",
          .func = cmd_sd },
        { .command = "flash", .help = "Internal FAT: status | mount | init (formats if unmountable)",
          .func = cmd_flash },
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
