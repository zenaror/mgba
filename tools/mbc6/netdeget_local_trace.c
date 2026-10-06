/* Fixture-only Net de Get integration tracer.
 * Uses public synthetic adapter credentials and loopback services only.
 * SPDX-License-Identifier: MPL-2.0
 */
#include <mgba/flags.h>
#include <mgba/core/core.h>
#include <mgba/core/version.h>
#include <mgba/core/config.h>
#include <mgba/core/directories.h>
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/sm83/sm83.h>
#include <mgba-util/vfs.h>
#include <mgba-util/image.h>
#include <mgba/internal/gb/sio/mobile.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char* traceDir;
static struct GBSIOMobileAdapter mobile;
static FILE* captureFile(const char* name) {
 char path[1024];snprintf(path,sizeof(path),"%s/%s",traceDir,name);return fopen(path,"ab");
}
static int localConnect(void* user,unsigned conn,const struct mobile_addr* addr) {
 struct MobileAdapterGB*m=user;
 if(addr->type!=MOBILE_ADDRTYPE_IPV4)return -1;
 const struct mobile_addr4*a=(const struct mobile_addr4*)addr;
 if(a->port!=80)return -1; /* fixture-only: no external destinations */
 struct Address dst={.version=IPV4,.ipv4=0x7F000001};
 int rc=SocketConnect(m->socket[conn].fd,8088,&dst);
 if(!rc||SocketIsConnected())return 1;
 return SocketIsConnecting()?0:-1;
}
static int captureSend(void* user, unsigned conn, const void* data, unsigned size, const struct mobile_addr* addr) {
	struct MobileAdapterGB* mobile = user;

	struct Address sendaddr;
	int destport = 0;
	struct Address* destaddr = NULL;

	if (addr) {
		if (addr->type == MOBILE_ADDRTYPE_IPV6) {
			const struct mobile_addr6* addr6 = (struct mobile_addr6*) addr;
			sendaddr.version = IPV6;
			memcpy(&sendaddr.ipv6, addr6->host, MOBILE_HOSTLEN_IPV6);
			destaddr = &sendaddr;
			destport = addr6->port;
		} else {
			const struct mobile_addr4* addr4 = (struct mobile_addr4*) addr;
			sendaddr.version = IPV4;
			sendaddr.ipv4 = ntohl(*(uint32_t*) &addr4->host);
			destaddr = &sendaddr;
			destport = addr4->port;
		}
	}

	ssize_t res = SocketSendTo(mobile->socket[conn].fd, data, size, destport, destaddr);
	if(res>0 && !addr){FILE*f=captureFile("tcp-send.bin");if(f){fwrite(data,1,res,f);fclose(f);}}
 return !SOCKET_RESERROR(res) ? res : -1;
}


static int captureRecv(void* user, unsigned conn, void* data, unsigned size, struct mobile_addr* addr) {
	struct MobileAdapterGB* mobile = user;

	// No polling first: the socket is non-blocking, so the read below already
	// answers "has anything arrived" by itself, and it answers honestly on
	// services whose polling does not.
	struct Address srcaddr = {0};
	int srcport = 0;
	ssize_t res = SocketRecvFrom(mobile->socket[conn].fd, data, size, &srcport, &srcaddr);
	if (SOCKET_RESERROR(res)) {
		return SocketWouldBlock() ? 0 : -1;
	}

	if(res>0 && mobile->socket[conn].socktype!=MOBILE_SOCKTYPE_UDP){FILE*f=captureFile("tcp-recv.bin");if(f){fwrite(data,1,res,f);fclose(f);}}
 if (res > 0 && addr) {
		if (srcaddr.version == IPV6) {
			struct mobile_addr6* addr6 = (struct mobile_addr6*) addr;
			addr6->type = MOBILE_ADDRTYPE_IPV6;
			memcpy(&addr6->host, &srcaddr.ipv6, MOBILE_HOSTLEN_IPV6);
			addr6->port = srcport;
		} else {
			struct mobile_addr4* addr4 = (struct mobile_addr4*) addr;
			addr4->type = MOBILE_ADDRTYPE_IPV4;
			*(uint32_t*) &addr4->host = htonl(srcaddr.ipv4);
			addr4->port = srcport;
		}
	}

	return (res || (mobile->socket[conn].socktype == MOBILE_SOCKTYPE_UDP)) ? res : -2;
}


/* Dan Docs/Trainer SDK EEPROM layout; only public test account data. */
static void localSetup(struct MobileAdapterGB* m) {
    memset(m->config, 0, 0xC0);
    m->config[0] = 'M';
    m->config[1] = 'A';
    m->config[2] = 0x81;
    memcpy(m->config + 0x0C, "g000000007", 10);
    memset(m->config + 0x76, 0xFF, 8);
    m->config[0x76] = 0xA9; /* BCD #9677, F terminator */
    m->config[0x77] = 0x67;
    m->config[0x78] = 0x7F;
    memcpy(m->config + 0x7E, "DION", 4);
    unsigned sum = 0;
    for (unsigned i = 0; i < 0xBE; ++i) sum += m->config[i];
    m->config[0xBE] = sum >> 8;
    m->config[0xBF] = sum;
    mobile_config_load(m->adapter);
    mobile_config_set_device(m->adapter, MOBILE_ADAPTER_BLUE, true);
    struct mobile_addr4 dns = {.type = MOBILE_ADDRTYPE_IPV4, .port = 8053, .host = {127, 0, 0, 1}};
    mobile_config_set_dns(m->adapter, (const struct mobile_addr*) &dns, MOBILE_DNS1);
    mobile_config_set_dns(m->adapter, (const struct mobile_addr*) &dns, MOBILE_DNS2);
    mobile_def_sock_connect(m->adapter, localConnect);
    mobile_def_sock_send(m->adapter, captureSend);
    mobile_def_sock_recv(m->adapter, captureRecv);
    mobile_config_save(m->adapter);
}

static void snapshot(struct mCore* core, const char* dir, int stage) {
    unsigned w, h;
    core->currentVideoSize(core, &w, &h);
    const void* pixels;
    size_t stride;
    core->getPixels(core, &pixels, &stride);
    char path[1024];
    snprintf(path, sizeof(path), "%s/stage-%02d.ppm", dir, stage);
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); exit(8); }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    const mColor* pix = pixels;
    for (unsigned y = 0; y < h; ++y) {
        for (unsigned x = 0; x < w; ++x) {
            mColor value = pix[y * stride + x]; /* stride is pixels, not bytes */
            fputc(M_R8(value), f);
            fputc(M_G8(value), f);
            fputc(M_B8(value), f);
        }
    }
    fclose(f);
    struct GB* gb = core->board;
    snprintf(path, sizeof(path), "%s/stage-%02d.wram", dir, stage);
    f = fopen(path, "wb");
    if (!f) { perror(path); exit(8); }
    fwrite(gb->memory.wram, 1, 0x8000, f);
    fclose(f);
    printf("stage=%d frame=%u pc=%04X A=%u B=%u fixtureFrames=%u held=%02X pressed=%02X counters=",
           stage, core->frameCounter(core), gb->cpu->pc, gb->memory.currentBank,
           gb->memory.currentBank1, core->busRead8(core, 0xD800),
           core->busRead8(core, 0xD802), core->busRead8(core, 0xD801));
    for (int i = 0; i < 8; ++i) printf("%02X", core->busRead8(core, 0xD803 + i));
    printf(" exitCode=%02X IE=%02X IF=%02X IME=%d FF8A=%02X joy=%02X STAT=%02X LCDC=%02X LY=%02X SP=%04X halted=%d pending=%d FF8E=%02X FF8F=%02X\n",
           core->busRead8(core, 0xC671), core->busRead8(core, 0xFFFF),
           core->busRead8(core, 0xFF0F), gb->memory.ime, core->busRead8(core, 0xFF8A),
           core->busRead8(core, 0xFF96), core->busRead8(core, 0xFF41),
           core->busRead8(core, 0xFF40), core->busRead8(core, 0xFF44), gb->cpu->sp,
           gb->cpu->halted, gb->cpu->irqPending, core->busRead8(core, 0xFF8E),
           core->busRead8(core, 0xFF8F));
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s ROM FIXTURE_DIR KEY_MASK:FRAMES...\n", argv[0]);
        return 2;
    }
    traceDir = argv[2];
    printf("version=%s commit=%s\n", projectVersion, gitCommit);
    struct mCore* core = mCoreFind(argv[1]);
    if (!core || !core->init(core)) return 3;
    mCoreInitConfig(core, "natural-pad-test");
    if (!mCoreLoadFile(core, argv[1])) return 4;
    core->dirs.save = VDirOpen(argv[2]);
    if (!core->dirs.save) return 5;
    strcpy(core->dirs.baseName, "padtest");
    if (!mCoreAutoloadSave(core)) return 5;
    mColor* video = calloc(256 * 256, sizeof(mColor));
    if (!video) return 5;
    core->setVideoBuffer(core, video, 256);
    core->reset(core);
    if (getenv("LOCAL_MOBILE")) {
        GBSIOMobileAdapterCreate(&mobile);
        mobile.m.setup = localSetup;
        GBSIOSetDriver(&((struct GB*) core->board)->sio, &mobile.d);
        if (!mobile.m.adapter) return 7;
    }
    for (int i = 3; i < argc; ++i) {
        unsigned key, frames;
        if (sscanf(argv[i], "%u:%u", &key, &frames) != 2 || key > 255) return 6;
        core->setKeys(core, key);
        for (unsigned j = 0; j < frames; ++j) core->runFrame(core);
        snapshot(core, argv[2], i - 3);
    }
    core->setKeys(core, 0);
    core->unloadROM(core);
    mCoreConfigDeinit(&core->config);
    core->deinit(core);
    free(video);
    return 0;
}
