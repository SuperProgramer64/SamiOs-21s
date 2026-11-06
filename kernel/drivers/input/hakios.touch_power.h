/* SPDX-License-Identifier: MIT
 * Exynos850 ACPM I3C mailbox and S2MPU12 LDO28, independently implemented.
 * Register/protocol reference: U-Boot board/samsung/e850-96/{acpm,pmic}.c
 * (Sam Protsenko, Linaro). Only LDO28 enable bits may change; never its selector.
 */
#ifndef HAKIOS_TOUCH_POWER_H
#define HAKIOS_TOUCH_POWER_H
struct hakios_touch_power_io {
    void *context;
    uint32_t (*read)(void *,unsigned,uint32_t);
    void (*write)(void *,unsigned,uint32_t,uint32_t);
    uint64_t (*nanoseconds)(void *);
    void (*relax)(void *);
};
static int hakios_touch_power_transfer(struct hakios_touch_power_io *io,int writing,uint8_t *v)
{
    void *c=io->context;const uint32_t bit=1u<<10;
    /* Never consume another pending transaction or overwrite its response. */
    if(io->read(c,0,0x28)&bit)return -10;
    io->write(c,1,0,0x146); /* channel 0, PM slave 1, LDO28 CTRL 0x46 */
    io->write(c,1,4,writing?(1u|((uint32_t)*v<<8)):0);
    io->write(c,0,8,bit<<16);
    uint64_t until=io->nanoseconds(c)+100000000ULL;
    while(!(io->read(c,0,0x28)&bit)) {
        if(io->nanoseconds(c)>=until)return -11;
        io->relax(c);
    }
    uint32_t reply=io->read(c,1,12);
    io->write(c,0,0x20,bit);
    if(reply>>24)return -12;
    if(!writing)*v=(reply>>8)&255;
    return 0;
}
static int hakios_touch_power(struct hakios_touch_power_io *io,unsigned mode,uint8_t *before,uint8_t *after)
{
    uint8_t old=0,again=0;int e;
    if(mode!=1&&mode!=2)return -13;
    if((e=hakios_touch_power_transfer(io,0,&old)))return e;
    *before=*after=old;
    if(old==255)return -14; /* an implausible all-ones response: refuse writes */
    if(mode==1||(old&0xc0)==0xc0)return 0;
    if((e=hakios_touch_power_transfer(io,0,&again)))return e;
    if(again!=old)return -15;
    uint8_t wanted=old|0xc0;
    if((e=hakios_touch_power_transfer(io,1,&wanted)))return e;
    if((e=hakios_touch_power_transfer(io,0,after)))return e;
    return *after==wanted?0:-16;
}
#endif
