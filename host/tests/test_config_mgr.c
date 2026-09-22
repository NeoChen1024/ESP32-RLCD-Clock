#include "config_mgr.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char sd[] = "/tmp/cfg-sd-XXXXXX", flash[] = "/tmp/cfg-f-XXXXXX";
static bool sd_mounted = true, flash_mounted = true;
static int effective_tz;
static char effective_ntp[64];
const char *storage_root(const char *v) { return !strcmp(v,"sd") ? sd : flash; }
bool storage_mounted_locked(const char *v) { return !strcmp(v,"sd") ? sd_mounted : flash_mounted; }
bool storage_lock(unsigned ms) { (void)ms; return true; }
void storage_unlock(void) {}
void model_tz_set_config(int minutes) { effective_tz=minutes; }
void sntp_mgr_set_config_server(const char *server) { snprintf(effective_ntp,sizeof effective_ntp,"%s",server?server:""); }
static void put(const char *root, const char *name, const char *body)
{
    char path[180]; snprintf(path,sizeof path,"%s/config/%s",root,name);
    FILE *f=fopen(path,"wb");assert(f);
    assert(fwrite(body,1,strlen(body),f)==strlen(body));assert(!fclose(f));
}
static void selection(const char *volume, const char *path, int tz, const char *ntp)
{
    config_selection_t current;
    config_mgr_current_locked(&current);
    assert(current.found && !strcmp(current.volume,volume) && !strcmp(current.path,path));
    assert(current.tz_offset_minutes==tz && effective_tz==tz && !strcmp(effective_ntp,ntp));
}
int main(void)
{
    assert(mkdtemp(sd) && mkdtemp(flash));
    char dir[180];snprintf(dir,sizeof dir,"%s/config",sd);assert(!mkdir(dir,0700));
    snprintf(dir,sizeof dir,"%s/config",flash);assert(!mkdir(dir,0700));
    put(flash,"20260923T090000000Z.json","{\"tz_offset_minutes\":120,\"ntp_server\":\"time.example.org\"}");
    put(sd,"20260923T080000000Z.json","{\"tz_offset_minutes\":60}");
    put(sd,"20260923T100000000Z.json","{\"tz_offset_minutes\":\"bad\"}");
    config_mgr_start();
    selection("sd","config/20260923T080000000Z.json",60,"");
    put(sd,"20260923T110000000Z.json","{\"ntp_server\":\"pool.ntp.org\",\"tz_offset_minutes\":-300}");
    assert(config_mgr_reload());
    selection("sd","config/20260923T110000000Z.json",-300,"pool.ntp.org");
    put(sd,"20260923T120000000Z.json","{\"ntp_server\":\"bad host\"}");
    assert(config_mgr_reload());
    selection("sd","config/20260923T110000000Z.json",-300,"pool.ntp.org");
    sd_mounted=false;
    assert(config_mgr_reload());
    selection("flash","config/20260923T090000000Z.json",120,"time.example.org");
    flash_mounted=false;
    assert(!config_mgr_reload());
    config_selection_t current;config_mgr_current_locked(&current);
    assert(!current.found && effective_tz==480 && !effective_ntp[0]);
    const char *sdnames[]={"20260923T080000000Z.json","20260923T100000000Z.json","20260923T110000000Z.json","20260923T120000000Z.json"};
    for(unsigned i=0;i<4;++i){snprintf(dir,sizeof dir,"%s/config/%s",sd,sdnames[i]);assert(!unlink(dir));}
    snprintf(dir,sizeof dir,"%s/config",sd);assert(!rmdir(dir));assert(!rmdir(sd));
    snprintf(dir,sizeof dir,"%s/config/20260923T090000000Z.json",flash);assert(!unlink(dir));
    snprintf(dir,sizeof dir,"%s/config",flash);assert(!rmdir(dir));assert(!rmdir(flash));
    puts("SD priority, newest valid config, fallback to prior version and flash OK");
}
