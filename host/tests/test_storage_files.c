#include "storage_files.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cJSON.h"

/* Relative to the test working directory (host/build): writable under
 * sandboxes, and short enough for the 24-character storage root limit. */
static char root[] = "./rlcd-fs-XXXXXX";
static const char *version = "config/20260923T010000000Z.json";
static void path(char out[256], const char *name) { snprintf(out, 256, "%s/%s", root, name); }
static void write_file(const char *name, const char *data)
{
    char p[256]; path(p, name); FILE *f = fopen(p, "wb"); assert(f);
    assert(fwrite(data, 1, strlen(data), f) == strlen(data)); assert(!fclose(f));
}
static void expect_file(const char *name, const char *expected)
{
    char p[256], data[256] = {0}; path(p, name); FILE *f = fopen(p, "rb"); assert(f);
    size_t n = fread(data, 1, sizeof data, f); assert(n == strlen(expected));
    assert(!memcmp(data, expected, n)); assert(!fclose(f));
}
static void begin(const char *relative, const char *content)
{
    int fd = storage_txn_begin(root, relative); assert(fd >= 0);
    assert(write(fd, content, strlen(content)) == (ssize_t)strlen(content));
    assert(!fsync(fd)); assert(!close(fd));
}
static void move(const char *from, const char *to)
{
    char a[256], b[256]; path(a, from); path(b, to); assert(!rename(a, b));
}
static bool valid_config(const char *absolute, void *context)
{
    (void)context;
    FILE *f = fopen(absolute, "rb"); if (!f) return false;
    char text[128]; size_t n = fread(text, 1, sizeof text - 1, f); bool ok = !ferror(f);
    if (fclose(f)) ok = false;
    text[n] = 0;
    cJSON *json = ok ? cJSON_ParseWithLengthOpts(text, n + 1, NULL, true) : NULL;
    ok = cJSON_IsObject(json) && cJSON_GetObjectItemCaseSensitive(json, "timezone") != NULL;
    cJSON_Delete(json); return ok;
}
int main(void)
{
    char volume[8], rel[STORAGE_REL_MAX];
    assert(storage_parse_uri("/fs/sd/config/20260923T010000000Z.json", volume, rel));
    assert(!strcmp(volume,"sd") && !strcmp(rel,version));
    assert(storage_parse_uri("/fs/flash/sounds/alarm%20one.wav", volume, rel));
    assert(!strcmp(rel,"sounds/alarm one.wav"));
    assert(storage_parse_uri("/fs/sd/sounds/alarm.flac", volume, rel));
    assert(storage_parse_uri("/fs/flash/", volume, rel));
    assert(storage_parse_uri("/fs/sd/config/", volume, rel));
    assert(storage_parse_uri("/fs/sd/time/", volume, rel));
    assert(storage_parse_uri("/fs/flash/time/leap-seconds.list", volume, rel) && !strcmp(rel, STORAGE_LEAP_FILE));
    assert(storage_parse_uri("/fs/sd/secrets/", volume, rel));
    assert(storage_parse_uri("/fs/sd/secrets/wifi.json", volume, rel) && !strcmp(rel, STORAGE_WIFI_SECRETS));
    const char *bad[] = {"/fs/sd/config.json", "/fs/sd/../config/foo.json",
        "/fs/sd/config/%2e%2e/foo.json", "/fs/sd/sounds%2falarm.wav",
        "/fs/sd/sounds/%00alarm.wav", "/fs/sd/sounds/a%5cb.wav", "/fs/sd/sounds/a%252fb.wav",
        "/fs/sd/config/../config/foo.json", "/fs/sd/.rlcd-txn/record", "/fs/sd/sounds/ALARM~1.WAV",
        "/fs/sd/config/x.json?x=1", "/fs/other/config/x.json", "/fs/sd//config/x.json",
        "/fs/sd/sounds/%", "/fs/sd/sounds/a%0d.wav", "/fs/sd/sounds/.hidden.wav",
        "/fs/sd/sounds/sub/a.wav", "/fs/sd/sounds/a.FLAC", "/fs/sd/sounds/a.mp3", "/fs/sd/time/other.list", "/fs/sd/time/leap-seconds.list.bak",
        "/fs/sd/time/sub/leap-seconds.list", "/fs/sd/secrets/other.json", "/fs/sd/secrets/wifi.json.bak",
        "/fs/sd/cleanup"};
    for (unsigned i = 0; i < sizeof bad/sizeof bad[0]; ++i) assert(!storage_parse_uri(bad[i],volume,rel));
    assert(mkdtemp(root));
    assert(!storage_txn_recover(root));
    begin(version,"{\"timezone\":480}"); assert(!storage_txn_commit(root)); expect_file(version,"{\"timezone\":480}");
    begin(version,"{\"timezone\":120}"); assert(!storage_txn_commit(root)); expect_file(version,"{\"timezone\":120}");
    begin(version,"{\"incomplete\":"); assert(!storage_txn_recover(root)); expect_file(version,"{\"timezone\":120}");
    const char *invalid[] = {"[]", "{} garbage", "{", "null", "{\"a\":true,}"};
    for (unsigned i = 0; i < sizeof invalid/sizeof invalid[0]; ++i) {
        begin(version,invalid[i]); assert(storage_txn_commit(root) == -1 && errno == EINVAL);
        assert(!storage_txn_recover(root)); expect_file(version,"{\"timezone\":120}");
    }
    begin(version,"{\"timezone\":240}"); move(version, ".rlcd-txn/backup");
    assert(!storage_txn_recover(root)); expect_file(version,"{\"timezone\":120}");
    begin(version,"{\"timezone\":360}"); move(version, ".rlcd-txn/backup"); move(".rlcd-txn/upload", version);
    assert(!storage_txn_recover(root)); expect_file(version,"{\"timezone\":360}");
    write_file(".rlcd-txn/record", "not our journal");
    assert(storage_txn_recover(root) == -1); expect_file(".rlcd-txn/record", "not our journal");
    char p[256]; path(p,".rlcd-txn/record"); assert(!unlink(p));
    write_file(".rlcd-txn/upload", "unknown file"); assert(storage_txn_recover(root)==-1);
    expect_file(".rlcd-txn/upload","unknown file");path(p,".rlcd-txn/upload");assert(!unlink(p));
    char deep[256]; size_t n=0;
    for(int i=0;i<20;++i){memcpy(deep+n,"{\"a\":",5);n+=5;}deep[n++]='0';for(int i=0;i<20;++i)deep[n++]='}';deep[n]=0;
    begin(version,deep);assert(storage_txn_commit(root)==-1 && errno==EINVAL);assert(!storage_txn_recover(root));
    begin("config/20260923T020000000Z.json", "{\"timezone\":240}");assert(!storage_txn_commit(root));
    write_file("config/20260923T030000000Z.json", "corrupt JSON");
    char selected[STORAGE_REL_MAX];
    assert(storage_config_select(root,valid_config,NULL,selected)==1);
    assert(!strcmp(selected,"config/20260923T020000000Z.json"));
    write_file("config/20260923T020000000Z.json", "{\"unknown\":true}");
    assert(storage_config_select(root,valid_config,NULL,selected)==1 && !strcmp(selected,version));
    int fd=storage_txn_begin(root,"sounds/test.wav");assert(fd>=0);
    /* 16-bit stereo 44.1 kHz PCM with one frame of data. */
    const unsigned char wav[]={ 'R','I','F','F',40,0,0,0,'W','A','V','E',
        'f','m','t',' ',16,0,0,0, 1,0, 2,0, 0x44,0xac,0,0, 0x10,0xb1,2,0, 4,0, 16,0,
        'd','a','t','a',4,0,0,0, 1,2,3,4 };
    assert(write(fd,wav,sizeof wav)==sizeof wav);assert(!fsync(fd));assert(!close(fd));assert(!storage_txn_commit(root));
    fd=storage_txn_begin(root,"sounds/test.wav");assert(fd>=0);
    assert(write(fd,wav,12)==12);assert(!fsync(fd));assert(!close(fd));
    assert(storage_txn_commit(root)==-1 && errno==EINVAL); /* header only: not playable */
    assert(!storage_txn_recover(root));
    path(p,"sounds/test.wav");assert(!unlink(p));path(p,"sounds");assert(!rmdir(p));
    /* The leap table replaces in place only when its integrity hash verifies. */
    const char leap[] = "#$\t100\n#@\t4000000000\n2272060800\t10\n2287785600\t11\n"
                        "#h\t5367a011 c30a5f81 9340d986 82856c05 33c8c286\n";
    begin(STORAGE_LEAP_FILE, leap); assert(!storage_txn_commit(root)); expect_file(STORAGE_LEAP_FILE, leap);
    begin(STORAGE_LEAP_FILE, "#$\t101\n#@\t4000000000\n2272060800\t10\n#h\t1 2 3 4 5\n");
    assert(storage_txn_commit(root) == -1 && errno == EINVAL);
    assert(!storage_txn_recover(root)); expect_file(STORAGE_LEAP_FILE, leap);
    path(p,STORAGE_LEAP_FILE);assert(!unlink(p));path(p,"time");assert(!rmdir(p));
    const char *configs[] = {version,"config/20260923T020000000Z.json","config/20260923T030000000Z.json"};
    for (unsigned i=0;i<3;++i){path(p,configs[i]);assert(!unlink(p));}
    path(p,"config");assert(!rmdir(p));path(p,".rlcd-txn");assert(!rmdir(p));assert(!rmdir(root));
    puts("version ordering, fallback, storage paths and interrupted replacement recovery OK");
}
