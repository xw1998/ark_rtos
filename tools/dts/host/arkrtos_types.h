/* Host shim for the device tree self-test: mirrors the target types but uses
 * fixed 32-bit widths so the generated tables have the same layout. */
#ifndef __ARKRTOS_TYPES_H__
#define __ARKRTOS_TYPES_H__
typedef unsigned int   uint32;
typedef signed   int   int32;
typedef unsigned short uint16;
typedef signed   short int16;
typedef unsigned char  uint8;
typedef signed   char  int8;
#define ARKRTOS_WEAK
#endif
