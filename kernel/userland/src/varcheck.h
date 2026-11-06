/* SPDX-License-Identifier: MIT
 * A boot-to-boot witness inside the mounted data volume. The caller supplies
 * private, root-owned paths in that volume. Never replace a corrupt witness:
 * that would hide lost/partial data. Atomic rename preserves the previous
 * generation until the new file has been completely written and fsynced.
 */
#ifndef HAKIOS_VARCHECK_H
#define HAKIOS_VARCHECK_H
static int hakios_var_check(const char *path, const char *tmp, const char *directory, uint64_t *previous)
{
    const uint64_t magic=0x31524156534f4948ULL;
    uint64_t data[4]={magic,0,~0ULL,magic};
    char *bytes=(char *)data, extra;
    size_t done=0; ssize_t n; int fd=open(path,O_RDONLY,0);
    *previous=0;
    if(fd<0) {
        if(errno!=2 /* ENOENT */) return -1;
    } else {
        while(done<sizeof(data) && (n=read(fd,bytes+done,sizeof(data)-done))>0) done+=(size_t)n;
        int valid=done==sizeof(data) && read(fd,&extra,1)==0 && data[0]==magic &&
            data[3]==magic && data[2]==~data[1] && data[1]!=~0ULL;
        close(fd);
        if(!valid) return -1;
        *previous=data[1];
    }
    data[1]++; data[2]=~data[1];
    /* Only our own uncommitted temporary name is discarded after interruption. */
    if(unlink(tmp)!=0 && errno!=2) return -2;
    fd=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0600);
    if(fd<0) return -2;
    done=0;
    while(done<sizeof(data) && (n=write(fd,bytes+done,sizeof(data)-done))>0) done+=(size_t)n;
    int ok=done==sizeof(data) && fsync(fd)==0;
    if(close(fd)!=0) ok=0;
    if(!ok) { unlink(tmp); return -2; }
    if(rename(tmp,path)!=0) { unlink(tmp); return -3; }
    fd=open(directory,O_RDONLY|O_DIRECTORY,0);
    if(fd<0) return -3;
    ok=fsync(fd)==0;
    if(close(fd)!=0) ok=0;
    return ok?0:-3;
}
#endif
