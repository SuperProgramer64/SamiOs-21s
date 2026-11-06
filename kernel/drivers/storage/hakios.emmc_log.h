/* SPDX-License-Identifier: MIT
 * HakiOS console records. All integers little-endian. A torn record is ignored.
 * 4096 bytes: magic[8], seq/u64, run/u64, stream position/u64, length/u32,
 * CRC32/u32 (computed with this field zero), reserved[24], payload[4032].
 */
#ifndef HAKIOS_EMMC_LOG_H
#define HAKIOS_EMMC_LOG_H
#define HAKIOS_EMMC_LOG_BYTES (4u*1024u*1024u)
#define HAKIOS_EMMC_LOG_BLOCK 4096u
#define HAKIOS_EMMC_LOG_DATA 4032u
static uint32_t hakios_log_u32(const uint8_t *p) {
 return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static uint64_t hakios_log_u64(const uint8_t *p) {
 return hakios_log_u32(p)|((uint64_t)hakios_log_u32(p+4)<<32);
}
static void hakios_log_put(uint8_t *p,uint64_t v,unsigned n) {
 for(unsigned i=0;i<n;i++){p[i]=(uint8_t)v;v>>=8;}
}
static uint32_t hakios_emmc_log_crc(const uint8_t *p) {
 uint32_t c=~0u;
 for(unsigned i=0;i<4096;i++) {
  c^=(i>=36&&i<40)?0:p[i];
  for(unsigned j=0;j<8;j++)c=(c>>1)^(0xedb88320u&(0u-(c&1)));
 }
 return ~c;
}
static int hakios_emmc_log_valid(const uint8_t *p) {
 const char magic[]="HAKLOG01";
 for(unsigned i=0;i<8;i++)if(p[i]!=(uint8_t)magic[i])return 0;
 uint32_t n=hakios_log_u32(p+32);
 return n>0&&n<=4032&&hakios_log_u64(p+8)>0&&hakios_log_u64(p+16)>0&&
  hakios_log_u32(p+36)==hakios_emmc_log_crc(p);
}
static int hakios_emmc_log_pack(uint8_t *p,uint64_t seq,uint64_t run,uint64_t pos,const uint8_t *data,uint32_t n) {
 if(!seq||!run||!n||n>4032)return 0;
 for(unsigned i=0;i<4096;i++)p[i]=0;
 const char magic[]="HAKLOG01";for(unsigned i=0;i<8;i++)p[i]=(uint8_t)magic[i];
 hakios_log_put(p+8,seq,8);hakios_log_put(p+16,run,8);hakios_log_put(p+24,pos,8);hakios_log_put(p+32,n,4);
 for(unsigned i=0;i<n;i++)p[64+i]=data[i];
 hakios_log_put(p+36,hakios_emmc_log_crc(p),4);return 1;
}
/* Exactly the final 4 MiB of the loader's 64 MiB payload reservation.
 * Root/var end before that reservation; payload header size must fit before log.
 */
static int hakios_emmc_log_window(uint64_t root,uint64_t room,uint64_t var_end,uint64_t var_size,
 uint64_t start,uint64_t sectors,uint64_t card,uint64_t payload_bytes) {
 const uint64_t reserve=131072,log=8192;
 if(!root||!room||!var_size||var_size>room||root>UINT64_MAX-room)return 0;
 uint64_t payload=root+room;
 if(payload>UINT64_MAX-reserve||var_end!=payload)return 0;
 uint64_t end=payload+reserve;
 return end<=card&&start==end-log&&sectors==log&&payload_bytes>=32&&
  payload_bytes<=(reserve-log)*512;
}
#endif
