#include "wifi_secrets.h"
#include "wifi_mgr.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Relative to the test working directory (host/build), which sandboxes
 * leave writable, unlike /tmp. */
static char sd[] = "./wifi-sd-XXXXXX", flash[] = "./wifi-f-XXXXXX";
static bool sd_mounted = true, flash_mounted = true;
static wifi_mgr_network_t known[WIFI_MGR_KNOWN_MAX];
static unsigned known_count = 99;
const char *storage_root(const char *v) { return !strcmp(v, "sd") ? sd : flash; }
bool storage_mounted_locked(const char *v) { return !strcmp(v, "sd") ? sd_mounted : flash_mounted; }
const char *storage_active_volume_locked(void) { return sd_mounted ? "sd" : flash_mounted ? "flash" : NULL; }
bool wifi_mgr_set_known(const wifi_mgr_network_t *n, unsigned count)
{
    known_count = count;
    memcpy(known, n, sizeof *n * count);
    return true;
}

static void put(const char *root, const char *body)
{
    char path[180];
    snprintf(path, sizeof path, "%s/secrets", root);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/secrets/wifi.json", root);
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(body, 1, strlen(body), f) == strlen(body)); assert(!fclose(f));
}
static void put_path(const char *path, const char *body)
{
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(body, 1, strlen(body), f) == strlen(body)); assert(!fclose(f));
}
static void remove_file(const char *root)
{
    char path[180];
    snprintf(path, sizeof path, "%s/secrets/wifi.json", root); unlink(path);
    snprintf(path, sizeof path, "%s/secrets", root); rmdir(path);
}

int main(void)
{
    assert(mkdtemp(sd) && mkdtemp(flash));
    assert(wifi_secrets_file_valid(REPO_ROOT "/wifi.json.example"));

    char probe[180];
    snprintf(probe, sizeof probe, "%s/probe.json", sd);
    const char *bad[] = {
        "[]", "{}", "{\"networks\":{}}", "{\"networks\":[{}]}",
        "{\"networks\":[{\"ssid\":\"\"}]}",
        "{\"networks\":[{\"ssid\":\"123456789012345678901234567890123\"}]}",
        "{\"networks\":[{\"ssid\":\"a\",\"password\":\"short\"}]}",
        "{\"networks\":[{\"ssid\":\"a\",\"password\":\"12345678901234567890123456789012345678901234567890123456789012345\"}]}",
        "{\"networks\":[{\"ssid\":\"a\",\"password\":\"123456789012345678901234567890123456789012345678901234567890123x\"}]}",
        "{\"networks\":[{\"ssid\":\"a\",\"password\":\"tab\\tin password\"}]}",
        "{\"networks\":[{\"ssid\":\"a\",\"password\":12345678}]}",
        "{\"networks\":[{\"ssid\":\"dup\"},{\"ssid\":\"dup\",\"password\":\"password1\"}]}",
        "{\"networks\":[{\"ssid\":\"1\"},{\"ssid\":\"2\"},{\"ssid\":\"3\"},{\"ssid\":\"4\"},"
        "{\"ssid\":\"5\"},{\"ssid\":\"6\"},{\"ssid\":\"7\"},{\"ssid\":\"8\"},{\"ssid\":\"9\"}]}",
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        put_path(probe, bad[i]);
        if (wifi_secrets_file_valid(probe)) { printf("FAIL accepted %s\n", bad[i]); return 1; }
    }
    const char *good[] = {
        "{\"networks\":[]}",
        "{\"networks\":[{\"ssid\":\"open\",\"password\":null},{\"ssid\":\"open2\",\"password\":\"\"}]}",
        "{\"networks\":[{\"ssid\":\"hex\",\"password\":\"0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef\"}],\"comment\":1}",
        "{\"networks\":[{\"ssid\":\"12345678901234567890123456789012\",\"password\":\"exactly8\",\"note\":\"x\"}]}",
    };
    for (unsigned i = 0; i < sizeof good / sizeof good[0]; ++i) {
        put_path(probe, good[i]);
        if (!wifi_secrets_file_valid(probe)) { printf("FAIL rejected %s\n", good[i]); return 1; }
    }
    unlink(probe);

    /* Only the active volume counts: SD while mounted, else flash. */
    put(sd, "{\"networks\":[{\"ssid\":\"sd-net\",\"password\":\"password1\"},{\"ssid\":\"open\"}]}");
    put(flash, "{\"networks\":[{\"ssid\":\"flash-net\",\"password\":\"password2\"}]}");
    wifi_secrets_info_t info;
    assert(wifi_secrets_reload_locked());
    wifi_secrets_current_locked(&info);
    assert(info.found && !strcmp(info.volume, "sd") && info.count == 2 && known_count == 2);
    assert(!strcmp(known[0].ssid, "sd-net") && !strcmp(known[0].password, "password1"));
    assert(!strcmp(known[1].ssid, "open") && !known[1].password[0]);
    put(sd, "{\"networks\":[{\"ssid\":\"sd-net\",\"password\":\"short\"}]}");
    assert(!wifi_secrets_reload_locked());            /* invalid on SD: no fallback */
    wifi_secrets_current_locked(&info);
    assert(!info.found && known_count == 0);
    sd_mounted = false;
    assert(wifi_secrets_reload_locked());
    wifi_secrets_current_locked(&info);
    assert(!strcmp(info.volume, "flash") && known_count == 1 && !strcmp(known[0].ssid, "flash-net"));
    flash_mounted = false;
    assert(!wifi_secrets_reload_locked());
    wifi_secrets_current_locked(&info);
    assert(!info.found && known_count == 0);
    flash_mounted = sd_mounted = true;

    remove_file(sd); remove_file(flash);
    assert(!rmdir(sd) && !rmdir(flash));
    puts("wifi.json validation, example file and active-volume selection OK");
}
