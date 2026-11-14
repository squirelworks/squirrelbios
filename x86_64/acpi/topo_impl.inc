/* Shared MADT / SRAT / MCFG builders (included by topo.c and the host
 * test). Expects put16/put32/put64, acpi_header, acpi_seal, memset, and
 * the acpi_*_aff / acpi_mcfg_entry structs. Original code. */

u32 acpi_build_madt(u8 *t, const struct acpi_cpu_aff *cpus, u32 ncpus,
                    u32 lapic_base, u8 ioapic_id, u32 ioapic_base, int all_x2)
{
    u8 *p = t + 44;
    int x2 = 0;
    put32(t + 36, lapic_base);
    put32(t + 40, 1);                   /* PCAT_COMPAT: 8259s present */
    for (u32 i = 0; i < ncpus; i++) {
        u32 id = cpus[i].apic_id;
        u32 on = cpus[i].enabled ? 1u : 0u;
        if (!all_x2 && id < 255 && i < 255) {
            p[0] = 0;                   /* processor local APIC */
            p[1] = 8;
            p[2] = (u8)i;
            p[3] = (u8)id;
            put32(p + 4, on);
            p += 8;
        } else {
            p[0] = 9;                   /* processor local x2APIC */
            p[1] = 16;
            put16(p + 2, 0);
            put32(p + 4, id);
            put32(p + 8, on);
            put32(p + 12, i);
            p += 16;
            x2 = 1;
        }
    }
    p[0] = 1;
    p[1] = 12;
    p[2] = ioapic_id;
    p[3] = 0;
    put32(p + 4, ioapic_base);
    put32(p + 8, 0);                    /* GSI base */
    p += 12;
    static const u8 iso_src[] = { 0, 5, 9, 10, 11 };
    for (u32 i = 0; i < sizeof iso_src; i++) {
        p[0] = 2;
        p[1] = 10;
        p[2] = 0;
        p[3] = iso_src[i];
        put32(p + 4, iso_src[i] == 0 ? 2 : iso_src[i]);
        put16(p + 8, iso_src[i] == 0 ? 0 : 0x000D);
        p += 10;
    }
    p[0] = 4;                           /* local APIC NMI on LINT1, all CPUs */
    p[1] = 6;
    p[2] = 0xFF;
    put16(p + 3, 0);
    p[5] = 1;
    p += 6;
    if (x2) {
        p[0] = 0x0A;                    /* local x2APIC NMI on LINT1, all CPUs */
        p[1] = 12;
        put16(p + 2, 0);
        put32(p + 4, 0xFFFFFFFFu);
        p[8] = 1;
        p += 12;
    }
    u32 len = (u32)(p - t);
    acpi_header(t, "APIC", len, 5);
    acpi_seal(t);
    return len;
}

u32 acpi_build_srat(u8 *t, const struct acpi_cpu_aff *cpus, u32 ncpus,
                    const struct acpi_mem_aff *mems, u32 nmems, int all_x2)
{
    u8 *p = t + 48;
    put32(t + 36, 1);                   /* reserved, 1 for compatibility */
    for (u32 i = 0; i < ncpus; i++) {
        u32 id = cpus[i].apic_id;
        u32 node = cpus[i].prox;
        u32 on = cpus[i].enabled ? 1u : 0u;
        if (!all_x2 && id < 255) {
            p[0] = 0;                   /* processor local APIC affinity */
            p[1] = 16;
            p[2] = (u8)node;
            p[3] = (u8)id;
            put32(p + 4, on);
            p[8] = 0;
            p[9] = (u8)(node >> 8);
            p[10] = (u8)(node >> 16);
            p[11] = (u8)(node >> 24);
            put32(p + 12, 0);
            p += 16;
        } else {
            p[0] = 2;                   /* processor local x2APIC affinity */
            p[1] = 24;
            put16(p + 2, 0);
            put32(p + 4, node);
            put32(p + 8, id);
            put32(p + 12, on);
            put32(p + 16, 0);
            put32(p + 20, 0);
            p += 24;
        }
    }
    for (u32 i = 0; i < nmems; i++) {
        memset(p, 0, 40);
        p[0] = 1;                       /* memory affinity */
        p[1] = 40;
        put32(p + 2, mems[i].prox);
        put64(p + 8, mems[i].base);
        put64(p + 16, mems[i].length);
        put32(p + 28, 1);               /* enabled */
        p += 40;
    }
    u32 len = (u32)(p - t);
    acpi_header(t, "SRAT", len, 3);
    acpi_seal(t);
    return len;
}

u32 acpi_build_mcfg(u8 *t, const struct acpi_mcfg_entry *ents, u32 n)
{
    u32 len = 44 + 16 * n;
    memset(t + 36, 0, len > 36 ? len - 36 : 0);
    acpi_header(t, "MCFG", len, 1);
    for (u32 i = 0; i < n; i++) {
        u8 *p = t + 44 + 16 * i;
        put64(p, ents[i].base);
        put16(p + 8, ents[i].segment);
        p[10] = ents[i].start_bus;
        p[11] = ents[i].end_bus;
    }
    acpi_seal(t);
    return len;
}
