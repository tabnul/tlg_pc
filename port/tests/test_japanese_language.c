/* The Japanese-text view must look like missing French packs, without touching host files. */
#undef NDEBUG
#include "../src/runtime_file.c"
#include <assert.h>

static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
void runtime_wait_begin(const char *kind,const void *object,const char *name,const void *const *owner,uintptr_t frame) {
    (void)kind; (void)object; (void)name; (void)owner; (void)frame;
}
void runtime_wait_end(void) {}
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    (void)table; (void)count; (void)name; return 0;
}
static void make_file(const char *path) {
    FILE *f=fopen(path,"wb"); assert(f);
    assert(fputs("unchanged archive",f)>=0 && !fclose(f));
}
static void readable(const char *guest) {
    int fd=(int)do_open(guest,0,0); assert(fd>=3);
    char data[32]={0}; assert(do_read(fd,data,17)==17);
    assert(!strcmp(data,"unchanged archive") && !do_close(fd));
}
static int listed(const char *guest,const char *name) {
    int fd=(int)do_open(guest,0x20000,0); assert(fd>=3);
    char entries[4096]; int64_t count=do_getdents(fd,entries,sizeof(entries),NULL); assert(count>0);
    int found=0;
    for (int64_t at=0;at<count;) {
        uint16_t length; memcpy(&length,entries+at+4,2); assert(length);
        if (!strcmp(entries+at+8,name)) found=1;
        at+=length;
    }
    assert(!do_close(fd)); return found;
}
int main(void) {
    char root[512];
    snprintf(root,sizeof(root),"%s/tlg-jp-files-XXXXXX",getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    assert(mkdtemp(root));
    char game[600],user[600],path[800],nested[800];
    snprintf(game,sizeof(game),"%s/game",root);
    snprintf(user,sizeof(user),"%s/user",root);
    assert(!mkdir(game,0755));
    const char *packs[]={"trico_frf.psarc","trico_frfp.psarc","trico_boot.psarc","trico_frc.psarc"};
    for (size_t i=0;i<4;++i) { snprintf(path,sizeof(path),"%s/%s",game,packs[i]); make_file(path); }
    snprintf(nested,sizeof(nested),"%s/nested",game); assert(!mkdir(nested,0755));
    snprintf(path,sizeof(path),"%s/nested/trico_frf.psarc",game); make_file(path);
    unsetenv("TLG_JAPANESE_FALLBACK");
    runtime_file_configure(game,user);
    snprintf(path,sizeof(path),"%s/data/trico_frf.psarc",user); make_file(path);
    readable("/app0/trico_frf.psarc"); readable("/app0/trico_frfp.psarc");
    assert(listed("/app0","trico_frf.psarc") && listed("/hostapp","trico_frfp.psarc"));

    setenv("TLG_JAPANESE_FALLBACK","1",1);
    const char *hidden[]={"trico_frf.psarc","/app0/trico_frf.psarc","/hostapp/trico_frfp.psarc",
                          "./trico_frfp.psarc","/app0/./trico_frf.psarc","/app0//TRICO_FRF.PSARC"};
    for (size_t i=0;i<sizeof(hidden)/sizeof(*hidden);++i) {
        GuestStat info;
        assert(do_open(hidden[i],0,0)==-ENOENT);
        assert(do_stat(hidden[i],&info)==-ENOENT);
    }
    assert(!listed("/app0","trico_frf.psarc") && !listed("/hostapp","trico_frfp.psarc"));
    assert(!listed(".","trico_frf.psarc") && !listed("/app0/.","trico_frfp.psarc"));
    assert(listed("/app0","trico_boot.psarc") && listed("/app0","trico_frc.psarc"));
    readable("/app0/trico_boot.psarc"); readable("/app0/trico_frc.psarc");
    readable("/app0/nested/trico_frf.psarc"); readable("/data/trico_frf.psarc");
    assert(listed("/app0/nested","trico_frf.psarc") && listed("/data","trico_frf.psarc"));
    assert(do_open("/app0/nested/../trico_frf.psarc",0,0)==-EACCES);

    for (int i=0;i<2;++i) {
        setenv("TLG_JAPANESE_FALLBACK",i ? "invalid" : "0",1);
        readable("/app0/trico_frf.psarc"); readable("/app0/trico_frfp.psarc");
        assert(listed("/app0","trico_frf.psarc"));
    }
    unsetenv("TLG_JAPANESE_FALLBACK");
    readable("/hostapp/trico_frf.psarc");
    for (size_t i=0;i<4;++i) { snprintf(path,sizeof(path),"%s/%s",game,packs[i]); assert(!unlink(path)); }
    snprintf(path,sizeof(path),"%s/nested/trico_frf.psarc",game); assert(!unlink(path));
    assert(!rmdir(nested) && !rmdir(game));
    snprintf(path,sizeof(path),"%s/data/trico_frf.psarc",user); assert(!unlink(path));
    const char *dirs[]={"temp0","download0","data"};
    for (size_t i=0;i<3;++i) { snprintf(path,sizeof(path),"%s/%s",user,dirs[i]); assert(!rmdir(path)); }
    assert(!rmdir(user) && !rmdir(root));
    puts("Japanese text view: open/stat/list masking, isolation and reversible host-file preservation PASS");
}
