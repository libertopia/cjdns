/* vim: set expandtab ts=4 sw=4: */
/*
 * You may redistribute this program and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "interface/ETHInterface.h"
#include "exception/Except.h"
#include "wire/Message.h"
#include "wire/Ethernet.h"
#include "util/Assert.h"
#include "util/platform/Socket.h"
#include "util/events/Event.h"
#include "util/Identity.h"
#include "util/version/Version.h"

#include <ifaddrs.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <net/bpf.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <net/if.h>
#include <net/if_dl.h>

#define MAX_PACKET_SIZE 1496
#define MIN_PACKET_SIZE 46

#define PADDING 512

// single ethernet_frame
struct ethernet_frame
{
    uint8_t dest[6];
    uint8_t src[6];
    uint16_t type;
} Gcc_PACKED;
#define ethernet_frame_SIZE 14
Assert_compileTime(ethernet_frame_SIZE == sizeof(struct ethernet_frame));


struct ETHInterface_pvt
{
    struct ETHInterface pub;

    Socket socket;

    struct Log* logger;

    uint8_t myMac[6];

    String* ifName;

    uint8_t* buffer;
    int bufLen;

    Identity
};

static Iface_DEFUN sendMessage(struct Message* msg, struct Iface* iface)
{
    struct ETHInterface_pvt* ctx =
        Identity_containerOf(iface, struct ETHInterface_pvt, pub.generic.iface);

    struct Sockaddr* sa = (struct Sockaddr*) msg->bytes;
    Assert_true(Message_getLength(msg) >= Sockaddr_OVERHEAD);
    Assert_true(sa->addrLen <= ETHInterface_Sockaddr_SIZE);

    struct ETHInterface_Sockaddr sockaddr = { .generic = { .addrLen = 0 } };
    Er_assert(Message_epop(msg, &sockaddr, sa->addrLen));

    struct ETHInterface_Header hdr = {
        .version = ETHInterface_CURRENT_VERSION,
        .zero = 0,
        .length_be = Endian_hostToBigEndian16(Message_getLength(msg) + ETHInterface_Header_SIZE),
        .fc00_be = Endian_hostToBigEndian16(0xfc00)
    };
    Er_assert(Message_epush(msg, &hdr, ETHInterface_Header_SIZE));

    struct ethernet_frame ethFr = {
        .type = Ethernet_TYPE_CJDNS
    };
    if (sockaddr.generic.flags & Sockaddr_flags_BCAST) {
        Bits_memset(ethFr.dest, 0xff, 6);
    } else {
        Bits_memcpy(ethFr.dest, sockaddr.mac, 6);
    }
    Bits_memcpy(ethFr.src, ctx->myMac, 6);
    Er_assert(Message_epush(msg, &ethFr, ethernet_frame_SIZE));
  /*
    struct bpf_hdr bpfPkt = {
        .bh_caplen = Message_getLength(msg),
        .bh_datalen = Message_getLength(msg),
        .bh_hdrlen = BPF_WORDALIGN(sizeof(struct bpf_hdr))
    };
    Er_assert(Message_epush(msg, &bpfPkt, bpfPkt.bh_hdrlen));
*/
    if (Message_getLength(msg) != write(ctx->socket, msg->bytes, Message_getLength(msg))) {
        Log_debug(ctx->logger, "Error writing to eth device [%s]", strerror(errno));
    }
    return Error(NONE);
}

static void handleEvent2(struct ETHInterface_pvt* context,
                         uint8_t src[6],
                         uint8_t dst[6],
                         int length,
                         uint8_t* data,
                         struct Allocator* alloc)
{
    if (length < ETHInterface_Header_SIZE) {
        Log_debug(context->logger, "runt");
        return;
    }

    uint32_t contentLength = BPF_WORDALIGN(length - ETHInterface_Header_SIZE);
    struct Message* msg = Message_new(contentLength, PADDING, alloc);

    struct ETHInterface_Header hdr;
    Bits_memcpy(&hdr, data, ETHInterface_Header_SIZE);

    Bits_memcpy(msg->bytes, &data[ETHInterface_Header_SIZE], contentLength);

    // here we could put a switch statement to handle different versions differently.
    if (hdr.version != ETHInterface_CURRENT_VERSION) {
        Log_debug(context->logger, "DROP unknown version");
        return;
    }

    uint16_t reportedLength = Endian_bigEndianToHost16(hdr.length_be);
    reportedLength -= ETHInterface_Header_SIZE;
    if (Message_getLength(msg) != reportedLength) {
        if (Message_getLength(msg) < reportedLength) {
            Log_debug(context->logger, "DROP size field is larger than frame");
            return;
        }
        Er_assert(Message_truncate(msg, reportedLength));
    }
    if (hdr.fc00_be != Endian_hostToBigEndian16(0xfc00)) {
        Log_debug(context->logger, "DROP bad magic");
        return;
    }

    struct ETHInterface_Sockaddr sockaddr = { .zero = 0 };
    Bits_memcpy(sockaddr.mac, src, 6);
    sockaddr.generic.addrLen = ETHInterface_Sockaddr_SIZE;
    if (dst[0] == 0xff) {
        sockaddr.generic.flags |= Sockaddr_flags_BCAST;
    }

    Er_assert(Message_epush(msg, &sockaddr, ETHInterface_Sockaddr_SIZE));

    Assert_true(!((uintptr_t)msg->bytes % 4) && "Alignment fault");

    Iface_send(&context->pub.generic.iface, msg);
}

static void handleEvent(void* vcontext)
{
    struct ETHInterface_pvt* context = Identity_check((struct ETHInterface_pvt*) vcontext);
    ssize_t bytes = read(context->socket, context->buffer, context->bufLen);
    if (bytes < 0) {
        Log_debug(context->logger, "read(bpf, bpf_buf, buf_len) -> [%s]", strerror(errno));
    }
    if (bytes < 1) { return; }
    if (bytes < (ssize_t)sizeof(struct bpf_hdr)) {
        Log_debug(context->logger, "runt [%lld]", (long long) bytes);
        return;
    }
    int offset = 0;
    while (offset < bytes) {
        struct bpf_hdr* bpfPkt = (struct bpf_hdr*) &context->buffer[offset];
        struct ethernet_frame* ethFr =
            (struct ethernet_frame*) &context->buffer[offset + bpfPkt->bh_hdrlen];
        int frameLength = bpfPkt->bh_datalen;
        uint8_t* frameContent =
            (uint8_t*) &context->buffer[offset + bpfPkt->bh_hdrlen + ethernet_frame_SIZE];
        int contentLength = frameLength - ethernet_frame_SIZE;

        Assert_true(offset + bpfPkt->bh_hdrlen + frameLength <= bytes);
        Assert_true(Ethernet_TYPE_CJDNS == ethFr->type);

        struct Allocator* messageAlloc = Allocator_child(context->pub.generic.alloc);
        handleEvent2(context, ethFr->src, ethFr->dest, contentLength, frameContent, messageAlloc);
        Allocator_free(messageAlloc);

        offset += BPF_WORDALIGN(bpfPkt->bh_hdrlen + bpfPkt->bh_caplen);
    }
}

Er_DEFUN(List* ETHInterface_listDevices(struct Allocator* alloc))
{
    List* out = List_new(alloc);
    struct ifaddrs* ifaddr = NULL;
    if (getifaddrs(&ifaddr) || ifaddr == NULL) {
        Er_raise(alloc, "getifaddrs() -> errno:%d [%s]", errno, strerror(errno));
    }
    for (struct ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) {
        } else if (ifa->ifa_addr->sa_family != AF_LINK) {
        } else if (!(ifa->ifa_flags & IFF_UP)) {
        } else if (ifa->ifa_flags & IFF_LOOPBACK) {
        } else {
            List_addString(out, String_new(ifa->ifa_name, alloc), alloc);
        }
    }
    freeifaddrs(ifaddr);
    Er_ret(out);
}

static int closeSocket(struct Allocator_OnFreeJob* j)
{
    struct ETHInterface_pvt* ctx = Identity_check((struct ETHInterface_pvt*) j->userData);
    close(ctx->socket);
    return 0;
}

static Er_DEFUN(int openBPF(struct Allocator* alloc))
{
    for (int retry = 0; retry < 100; retry++) {
        for (int i = 0; i < 256; i++) {
            char buf[21] = { 0 };
            snprintf(buf, 20, "/dev/bpf%i", i);
            int bpf = open(buf, O_RDWR);
            if (bpf != -1) { Er_ret(bpf); }
        }
        // sleep for 0.1 seconds
        usleep(1000 * 100);
    }
    Er_raise(alloc, "Could not find available /dev/bpf device");
}

static Er_DEFUN(void macaddr(const char* ifname, uint8_t addrOut[6], struct Allocator* alloc))
{
    struct ifaddrs* ifa;
    if (getifaddrs(&ifa)) {
        Er_raise(alloc, "getifaddrs() -> [%s]", strerror(errno));
    } else {
        for (struct ifaddrs* ifap = ifa; ifap; ifap = ifap->ifa_next) {
            if (!strcmp(ifap->ifa_name, ifname) && ifap->ifa_addr->sa_family == AF_LINK) {
                Bits_memcpy(addrOut, LLADDR((struct sockaddr_dl*) ifap->ifa_addr), 6);
                freeifaddrs(ifa);
                Er_ret();
            }
        }
    }
    freeifaddrs(ifa);
    Er_raise(alloc, "Could not find mac address for [%s]", ifname);
    Er_ret();
}

Er_DEFUN(struct ETHInterface* ETHInterface_new(struct EventBase* eventBase,
                                      const char* bindDevice,
                                      struct Allocator* alloc,
                                      struct Log* logger))
{
    struct ETHInterface_pvt* ctx = Allocator_calloc(alloc, sizeof(struct ETHInterface_pvt), 1);
    Identity_set(ctx);
    ctx->pub.generic.iface.send = sendMessage;
    ctx->pub.generic.alloc = alloc;
    ctx->logger = logger;

    ctx->socket = Er(openBPF(alloc));

    Er(macaddr(bindDevice, ctx->myMac, alloc));

    struct ifreq ifr = { .ifr_name = { 0 } };
    CString_strcpy(ifr.ifr_name, bindDevice);
    if (ioctl(ctx->socket, BIOCSETIF, &ifr) > 0) {
        Er_raise(alloc, "ioctl(BIOCSETIF, [%s]) [%s]", bindDevice, strerror(errno));
    }

    // activate immediate mode (therefore, bufLen is initially set to "1")
    int bufLen = 1;
    if (ioctl(ctx->socket, BIOCIMMEDIATE, &bufLen) == -1) {
        Er_raise(alloc, "ioctl(BIOCIMMEDIATE) [%s]", strerror(errno));
    }

    // request buffer length
    if (ioctl(ctx->socket, BIOCGBLEN, &bufLen) == -1) {
        Er_raise(alloc, "ioctl(BIOCGBLEN) [%s]", strerror(errno));
    }
    Log_debug(logger, "ioctl(BIOCGBLEN) -> bufLen=%i", bufLen);
    ctx->buffer = Allocator_malloc(alloc, bufLen);
    ctx->bufLen = bufLen;

    // filter for cjdns ethertype (0xfc00)
    static struct bpf_insn cjdnsFilter[] = {
        BPF_STMT(BPF_LD+BPF_H+BPF_ABS, 12),
        BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, /* Ethernet_TYPE_CJDNS */ 0xfc00, 1, 0),
        // drop
        BPF_STMT(BPF_RET+BPF_K, 0),
        // How much of the packet to ask for...
        BPF_STMT(BPF_RET+BPF_K, ~0u)
    };
    struct bpf_program cjdnsFilterProgram = {
        .bf_len = (sizeof(cjdnsFilter) / sizeof(struct bpf_insn)),
        .bf_insns = cjdnsFilter,
    };
    if (ioctl(ctx->socket, BIOCSETF, &cjdnsFilterProgram) == -1) {
        Er_raise(alloc, "ioctl(BIOCSETF) [%s]", strerror(errno));
    }

    Socket_makeNonBlocking(ctx->socket);

    Event_socketRead(handleEvent, ctx, ctx->socket, eventBase, alloc);

    Allocator_onFree(alloc, closeSocket, ctx);

    Er_ret(&ctx->pub);
}
