#include "config_mgr.h"
#include "leap_mgr.h"
#include "wifi_secrets.h"
#include "audio_mgr.h"
#include "model.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char sd[] = "/tmp/cfg-sd-XXXXXX", flash[] = "/tmp/cfg-f-XXXXXX";
static bool sd_mounted = true, flash_mounted = true;
static char effective_tz[TZ_RULE_TEXT_MAX];
static unsigned leap_reloads;
static char effective_ntp[64];
const char *storage_root(const char *v) { return !strcmp(v,"sd") ? sd : flash; }
bool storage_mounted_locked(const char *v) { return !strcmp(v,"sd") ? sd_mounted : flash_mounted; }
bool storage_lock(unsigned ms) { (void)ms; return true; }
void storage_unlock(void) {}
void model_tz_set_config(const tz_rule_t *rule) { snprintf(effective_tz,sizeof effective_tz,"%s",rule?rule->text:MODEL_TZ_DEFAULT); }
bool leap_mgr_reload_locked(void) { ++leap_reloads; return false; }
bool wifi_secrets_reload_locked(void) { return false; }
static int effective_volume = -1;
void audio_mgr_set_config_volume(int volume) { effective_volume = volume; }
static void collect(const char *relative, void *context) { strcat(context, relative); strcat(context, ";"); }
void sntp_mgr_set_config_server(const char *server) { snprintf(effective_ntp,sizeof effective_ntp,"%s",server?server:""); }
static void put(const char *root, const char *name, const char *body)
{
    char path[180]; snprintf(path,sizeof path,"%s/config/%s",root,name);
    FILE *f=fopen(path,"wb");assert(f);
    assert(fwrite(body,1,strlen(body),f)==strlen(body));assert(!fclose(f));
}
static void selection(const char *volume, const char *path, const char *tz, const char *ntp)
{
    config_selection_t current;
    config_mgr_current_locked(&current);
    assert(current.found && !strcmp(current.volume,volume) && !strcmp(current.path,path));
    assert(!strcmp(current.tz.text,tz) && !strcmp(effective_tz,tz) && !strcmp(effective_ntp,ntp));
}
int main(void)
{
    assert(mkdtemp(sd) && mkdtemp(flash));
    assert(config_mgr_file_valid(REPO_ROOT "/config.json.example"));
    char dir[180];snprintf(dir,sizeof dir,"%s/config",sd);assert(!mkdir(dir,0700));
    snprintf(dir,sizeof dir,"%s/config",flash);assert(!mkdir(dir,0700));
    put(flash,"20260923T090000000Z.json","{\"tz_offset_minutes\":120,\"ntp_server\":\"time.example.org\"}");
    put(sd,"20260923T080000000Z.json","{\"tz_offset_minutes\":60}");
    put(sd,"20260923T100000000Z.json","{\"tz_offset_minutes\":\"bad\"}");
    config_mgr_start();
    selection("sd","config/20260923T080000000Z.json","<+01>-1","");
    assert(effective_volume == AUDIO_DEFAULT_VOLUME);
    assert(leap_reloads==1);
    put(sd,"20260923T110000000Z.json","{\"ntp_server\":\"pool.ntp.org\",\"tz_offset_minutes\":-300}");
    assert(config_mgr_reload());
    selection("sd","config/20260923T110000000Z.json","<-05>5","pool.ntp.org");
    put(sd,"20260923T120000000Z.json","{\"ntp_server\":\"bad host\"}");
    assert(config_mgr_reload());
    selection("sd","config/20260923T110000000Z.json","<-05>5","pool.ntp.org");
    /* "tz" is a POSIX rule and wins over the legacy offset; both must be valid. */
    put(sd,"20260923T130000000Z.json","{\"tz\":\"CET-1CEST,M3.5.0,M10.5.0/3\",\"tz_offset_minutes\":60,\"audio_volume\":35}");
    assert(config_mgr_reload());
    selection("sd","config/20260923T130000000Z.json","CET-1CEST,M3.5.0,M10.5.0/3","");
    assert(effective_volume == 35);
    put(sd,"20260923T140000000Z.json","{\"tz\":\"CST-8\",\"tz_offset_minutes\":9999}");
    put(sd,"20260923T150000000Z.json","{\"tz\":\"+08:00\"}");
    put(sd,"20260923T160000000Z.json","{\"tz\":480,\"audio_volume\":101}");
    assert(config_mgr_reload());
    selection("sd","config/20260923T130000000Z.json","CET-1CEST,M3.5.0,M10.5.0/3","");

    /* Cleanup removes only versions older than the volume's own selection;
     * newer invalid versions and the other volume are kept. */
    char kept[STORAGE_REL_MAX], seen[512] = "";
    assert(config_mgr_cleanup_locked("sd",false,kept,collect,seen)==4);
    assert(!strcmp(kept,"config/20260923T130000000Z.json"));
    assert(!strcmp(seen,"config/20260923T080000000Z.json;config/20260923T100000000Z.json;"
                        "config/20260923T110000000Z.json;config/20260923T120000000Z.json;"));
    snprintf(dir,sizeof dir,"%s/config/20260923T080000000Z.json",sd);
    assert(!access(dir,F_OK)); /* preview removes nothing */
    seen[0]=0;
    assert(config_mgr_cleanup_locked("sd",true,kept,collect,seen)==4);
    assert(access(dir,F_OK));
    selection("sd","config/20260923T130000000Z.json","CET-1CEST,M3.5.0,M10.5.0/3","");
    assert(config_mgr_cleanup_locked("sd",true,kept,NULL,NULL)==0);
    assert(config_mgr_cleanup_locked("flash",false,kept,NULL,NULL)==0);
    assert(!strcmp(kept,"config/20260923T090000000Z.json"));
    put(flash,"20260923T085000000Z.json","{\"tz\":\"bad\"}");
    snprintf(dir,sizeof dir,"%s/config/20260923T090000000Z.json",flash);
    assert(!rename(dir,"/tmp/rlcd-cfg-aside.json"));
    assert(config_mgr_cleanup_locked("flash",true,kept,NULL,NULL)==CONFIG_CLEANUP_NO_VALID && !kept[0]);
    snprintf(dir,sizeof dir,"%s/config/20260923T085000000Z.json",flash);
    assert(!access(dir,F_OK)); /* untouched without a valid version */
    assert(!unlink(dir));
    snprintf(dir,sizeof dir,"%s/config/20260923T090000000Z.json",flash);
    assert(!rename("/tmp/rlcd-cfg-aside.json",dir));
    sd_mounted=false;
    assert(config_mgr_cleanup_locked("sd",false,kept,NULL,NULL)==CONFIG_CLEANUP_UNMOUNTED);
    assert(config_mgr_reload());
    selection("flash","config/20260923T090000000Z.json","<+02>-2","time.example.org");
    flash_mounted=false;
    assert(!config_mgr_reload());
    config_selection_t current;config_mgr_current_locked(&current);
    assert(!current.found && !strcmp(effective_tz,MODEL_TZ_DEFAULT) && !effective_ntp[0]);
    const char *sdnames[]={"20260923T130000000Z.json","20260923T140000000Z.json","20260923T150000000Z.json","20260923T160000000Z.json"};
    for(unsigned i=0;i<4;++i){snprintf(dir,sizeof dir,"%s/config/%s",sd,sdnames[i]);assert(!unlink(dir));}
    snprintf(dir,sizeof dir,"%s/config",sd);assert(!rmdir(dir));assert(!rmdir(sd));
    snprintf(dir,sizeof dir,"%s/config/20260923T090000000Z.json",flash);assert(!unlink(dir));
    snprintf(dir,sizeof dir,"%s/config",flash);assert(!rmdir(dir));assert(!rmdir(flash));
    puts("SD priority, newest valid config, POSIX tz rules, fallback, flash and cleanup OK");
}
