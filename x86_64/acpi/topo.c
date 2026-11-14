/* SquirrelBIOS x86_64-uefi: topology-driven MADT, SRAT and MCFG builders
 * (roadmap F1-scale). Original code, written from the ACPI 6.5
 * specification (5.2.12 MADT, 5.2.16 SRAT, MCFG).
 *
 * The firmware gathers the live CPU, NUMA and ECAM topology and calls
 * these; the host-side test (tests/x86_64-tables.c) feeds a synthetic
 * topology of up to 1024 CPUs and several ECAM segments, then
 * tests/acpicheck.py checks the bytes. The bodies live in topo_impl.inc
 * so the host tool builds the same tables. */
#include "acpi.h"

static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, (u16)v); put16(p + 2, (u16)(v >> 16)); }
static void put64(u8 *p, u64 v) { put32(p, (u32)v); put32(p + 4, (u32)(v >> 32)); }

#include "topo_impl.inc"
