/**
* @file arpa/inet.h
* @brief Byte order and IPv4 text helpers for Ark RTOS Apps.
* @details All four conversions are userspace arithmetic - the kernel is not
 *          involved - so they behave exactly as the names promise and cost
 *          nothing but a few instructions.
* @note  inet_ntoa() returns a pointer to a per-call static buffer, as POSIX
 *        says it may. It is not reentrant and not thread safe: copy the
 *        result if you keep it.
* @author xw
*/
#ifndef __ARKRTOS_ARPA_INET_H__
#define __ARKRTOS_ARPA_INET_H__

#include "arkrtos_posix_types.h"
#include "netinet/in.h"

uint16 arkrtos_posix_htons(uint16 x);
uint16 arkrtos_posix_ntohs(uint16 x);
uint32 arkrtos_posix_htonl(uint32 x);
uint32 arkrtos_posix_ntohl(uint32 x);

/** "127.0.0.1" -> network order address, or INADDR_NONE on a bad string. */
in_addr_t    arkrtos_posix_inet_addr(const char *cp);
/** Network order address -> text, in a static buffer this call owns. */
char        *arkrtos_posix_inet_ntoa(struct in_addr in);
/** af must be AF_INET; returns 1 on success, 0 on a bad address, -1 with
 *  errno=ENOSYS for a family this layer does not have. */
int          arkrtos_posix_inet_pton(int af, const char *src, void *dst);
/** The reverse; dst_size must be at least 16 bytes for AF_INET. */
const char  *arkrtos_posix_inet_ntop(int af, const void *src, char *dst,
                                   socklen_t dst_size);

#define htons(x)                    arkrtos_posix_htons(x)
#define ntohs(x)                    arkrtos_posix_ntohs(x)
#define htonl(x)                    arkrtos_posix_htonl(x)
#define ntohl(x)                    arkrtos_posix_ntohl(x)
#define inet_addr(cp)               arkrtos_posix_inet_addr(cp)
#define inet_ntoa(in)               arkrtos_posix_inet_ntoa(in)
#define inet_pton(af, src, dst)     arkrtos_posix_inet_pton((af), (src), (dst))
#define inet_ntop(af, src, dst, n)  arkrtos_posix_inet_ntop((af), (src), (dst), (n))

#endif /* __ARKRTOS_ARPA_INET_H__ */
