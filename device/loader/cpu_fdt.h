/* SPDX-License-Identifier: MIT
 * Read CPU reg cells from the FDT actually handed over by S-Boot. No vendor
 * tree is embedded. Bounds are checked before every read, including strings.
 */
#ifndef HAKIOS_CPU_FDT_H
#define HAKIOS_CPU_FDT_H
#include <stdint.h>
#include <stddef.h>
static uint32_t hakios_fdt_be32(const uint8_t *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static int hakios_fdt_eq(const uint8_t *p, size_t n, const char *s)
{
    size_t i=0;
    while(i<n && s[i] && p[i]==(uint8_t)s[i]) i++;
    return i<n && !s[i] && !p[i];
}
static int hakios_fdt_cpu_ids(const void *blob, size_t available, uint64_t *ids, unsigned cap)
{
    const uint8_t *b=blob;
    if(available<40 || hakios_fdt_be32(b)!=0xd00dfeed) return -1;
    uint32_t total=hakios_fdt_be32(b+4), off=hakios_fdt_be32(b+8),
        so=hakios_fdt_be32(b+12), sn=hakios_fdt_be32(b+32), n=hakios_fdt_be32(b+36);
    if(total>available || total<40 || off<40 || (off&3) || off>total || n>total-off ||
        so<40 || so>total || sn>total-so) return -1;
    uint32_t at=off, end=off+n; int depth=-1, cpus=-1, count=0;
    unsigned cells=1; uint64_t reg=0; int have_reg=0, is_cpu=0; unsigned reg_len=0;
    while(at<end) {
        if(end-at<4) return -1;
        uint32_t token=hakios_fdt_be32(b+at); at+=4;
        if(token==1) {
            uint32_t start=at;
            while(at<end && b[at]) at++;
            if(at==end || depth>=31) return -1;
            depth++;
            if(depth==1 && hakios_fdt_eq(b+start,at-start+1,"cpus")) cpus=depth;
            if(cpus>=0 && depth==cpus+1) { have_reg=0; is_cpu=0; reg=0; reg_len=0; }
            at++; uint32_t pad=(-at)&3; if(pad>end-at) return -1; at+=pad;
        } else if(token==2) {
            if(depth<0) return -1;
            if(cpus>=0 && depth==cpus+1 && is_cpu) {
                if(!have_reg || reg_len!=cells*4 || (unsigned)count>=cap) return -1;
                for(int i=0;i<count;i++) if(ids[i]==reg) return -1;
                ids[count++]=reg;
            }
            if(depth==cpus) cpus=-1;
            depth--;
        } else if(token==3) {
            if(depth<0 || end-at<8) return -1;
            uint32_t len=hakios_fdt_be32(b+at), name=hakios_fdt_be32(b+at+4);at+=8;
            if(len>end-at || name>=sn) return -1;
            const uint8_t *key=b+so+name; size_t kn=sn-name;
            size_t z=0; while(z<kn && key[z]) z++; if(z==kn) return -1;
            if(cpus>=0 && depth==cpus && hakios_fdt_eq(key,kn,"#address-cells")) {
                if(len!=4) return -1; cells=hakios_fdt_be32(b+at);
                if(cells!=1 && cells!=2) return -1;
            }
            if(cpus>=0 && depth==cpus+1) {
                if(hakios_fdt_eq(key,kn,"reg")) {
                    if(have_reg) return -1;
                    reg_len=len;
                    if(len==4 || len==8) reg=hakios_fdt_be32(b+at);
                    if(len==8) reg=reg<<32 | hakios_fdt_be32(b+at+4);
                    have_reg=1;
                }
                if(hakios_fdt_eq(key,kn,"device_type")) is_cpu=hakios_fdt_eq(b+at,len,"cpu");
            }
            at+=len; uint32_t pad=(-at)&3; if(pad>end-at) return -1; at+=pad;
        } else if(token==4) { /* NOP */
        } else if(token==9) { return depth==-1 ? count : -1;
        } else return -1;
    }
    return -1;
}
#endif
