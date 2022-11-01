#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/char/serial.h"
#include "hw/misc/unimp.h"
#include "target/riscv/cpu.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/boot.h"
#include "hw/char/sifive_uart.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/intc/sifive_plic.h"
#include "chardev/char.h"
#include "sysemu/sysemu.h"
#include "hw/riscv/sifive_cpu.h"

#define TYPE_SOC "riscv.ee290c.oscibear.soc"
#define EE290C_SOC(obj) \
    OBJECT_CHECK(EE290CSoCState, (obj), TYPE_SOC)

typedef struct EE290CSoCState {
    /* Private */
    DeviceState parent_obj;

    /* Public */
    RISCVHartArrayState cpus;
    DeviceState *plic;
    MemoryRegion xip_mem;
} EE290CSoCState;

#define TYPE_EE290C_OSCIBEAR_MACHINE MACHINE_TYPE_NAME("ee290c_oscibear")
#define EE290C_OSCIBEAR_MACHINE(obj) \
    OBJECT_CHECK(EE290CMachineState, (obj), TYPE_EE290C_OSCIBEAR_MACHINE)

typedef struct EE290CMachineState {
    /*< private >*/
    MachineState parent_obj;

    /*< public >*/
    EE290CSoCState soc;
} EE290CMachineState;

enum {
    EE290C_OSCIBEAR_DEV_DEBUG,
    EE290C_OSCIBEAR_DEV_SYSTEM_CONTROL,
    EE290C_OSCIBEAR_DEV_ERROR,
    EE290C_OSCIBEAR_DEV_BOOT_ROM,
    EE290C_OSCIBEAR_DEV_TILE_RESET_CONTROL,
    EE290C_OSCIBEAR_DEV_CLINT,
    EE290C_OSCIBEAR_DEV_PLIC,
    EE290C_OSCIBEAR_DEV_LBWIF_RAM,
    EE290C_OSCIBEAR_DEV_QSPI_CONTROL,
    EE290C_OSCIBEAR_DEV_QSPI_XIP,
    EE290C_OSCIBEAR_DEV_UART0,
    EE290C_OSCIBEAR_DEV_DTIM,
};

static const MemMapEntry ee290c_oscibear_memmap[] = {
    [EE290C_OSCIBEAR_DEV_DEBUG]             = {        0x0,     0x1000 },
    [EE290C_OSCIBEAR_DEV_SYSTEM_CONTROL]    = {     0x2000,     0x1000 },
    [EE290C_OSCIBEAR_DEV_ERROR]             = {     0x3000,     0x1000 },
    [EE290C_OSCIBEAR_DEV_BOOT_ROM]          = {    0x10000,    0x10000 },
    [EE290C_OSCIBEAR_DEV_TILE_RESET_CONTROL]= {   0x100000,     0x1000 },
    [EE290C_OSCIBEAR_DEV_CLINT]             = {  0x2000000,    0x10000 },
    //XXX: PLIC should not be this large?
    [EE290C_OSCIBEAR_DEV_PLIC]              = {  0xc000000,  0x4000000 },
    [EE290C_OSCIBEAR_DEV_LBWIF_RAM]         = { 0x10000000,     0x1000 },
    [EE290C_OSCIBEAR_DEV_QSPI_CONTROL]      = { 0x10040000,     0x1000 },
    [EE290C_OSCIBEAR_DEV_QSPI_XIP]          = { 0x20000000,   0x100000 },
    [EE290C_OSCIBEAR_DEV_UART0]             = { 0x54000000,     0x1000 },
    [EE290C_OSCIBEAR_DEV_DTIM]              = { 0x80000000, 0x80008000 },
};

static void soc_init(Object *obj) {
    MachineState *ms = MACHINE(qdev_get_machine());
    EE290CSoCState *s = EE290C_SOC(obj);

    object_initialize_child(obj, "cpus", &s->cpus, TYPE_RISCV_HART_ARRAY);
    object_property_set_int(OBJECT(&s->cpus), "num-harts", ms->smp.cpus,
                            &error_abort);
    object_property_set_int(OBJECT(&s->cpus), "resetvec", ee290c_oscibear_memmap[EE290C_OSCIBEAR_DEV_DTIM].base, &error_abort);
}

#include "hw/riscv/sifive_e.h"
static void soc_realize(DeviceState *dev, Error **errp) {
    MachineState *ms = MACHINE(qdev_get_machine());
    EE290CSoCState *s = EE290C_SOC(dev);
    const MemMapEntry *memmap = ee290c_oscibear_memmap;
    MemoryRegion *sys_mem = get_system_memory();

    object_property_set_str(OBJECT(&s->cpus), "cpu-type", ms->cpu_type,
                            &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&s->cpus), &error_fatal);
    sifive_uart_create(sys_mem, memmap[EE290C_OSCIBEAR_DEV_UART0].base,
        serial_hd(0), NULL);

s->plic = sifive_plic_create(memmap[EE290C_OSCIBEAR_DEV_PLIC].base,
        (char *)SIFIVE_E_PLIC_HART_CONFIG, ms->smp.cpus, 0,
        SIFIVE_E_PLIC_NUM_SOURCES,
        SIFIVE_E_PLIC_NUM_PRIORITIES,
        SIFIVE_E_PLIC_PRIORITY_BASE,
        SIFIVE_E_PLIC_PENDING_BASE,
        SIFIVE_E_PLIC_ENABLE_BASE,
        SIFIVE_E_PLIC_ENABLE_STRIDE,
        SIFIVE_E_PLIC_CONTEXT_BASE,
        SIFIVE_E_PLIC_CONTEXT_STRIDE,
        memmap[EE290C_OSCIBEAR_DEV_PLIC].size);

    riscv_aclint_swi_create(memmap[EE290C_OSCIBEAR_DEV_CLINT].base,
        0, ms->smp.cpus, false);
    riscv_aclint_mtimer_create(memmap[EE290C_OSCIBEAR_DEV_CLINT].base +
            RISCV_ACLINT_SWI_SIZE,
        RISCV_ACLINT_DEFAULT_MTIMER_SIZE, 0, ms->smp.cpus,
        RISCV_ACLINT_DEFAULT_MTIMECMP, RISCV_ACLINT_DEFAULT_MTIME,
        RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ, false);
}

static void class_init(ObjectClass *oc, void *data) {
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = soc_realize;
    dc->user_creatable = false;
}

static const TypeInfo soc_type_info = {
    .name = TYPE_SOC,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(EE290CSoCState),
    .instance_init = soc_init,
    .class_init = class_init,
};

static void register_types(void) {
    type_register_static(&soc_type_info);
}

type_init(register_types)

static void machine_init(MachineState *machine) {
    const MemMapEntry *memmap = ee290c_oscibear_memmap;
    EE290CMachineState *s = EE290C_OSCIBEAR_MACHINE(machine);
    MemoryRegion *sys_mem = get_system_memory();

    /* Init SoC */
    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_SOC);
    qdev_realize(DEVICE(&s->soc), NULL, &error_fatal);

    /* DTIM */
    memory_region_add_subregion(sys_mem,
        memmap[EE290C_OSCIBEAR_DEV_DTIM].base, machine->ram);

    if (machine->kernel_filename) {
        riscv_load_kernel(machine->kernel_filename,
                          memmap[EE290C_OSCIBEAR_DEV_DTIM].base, NULL);
    }
}

static void machine_instance_init(Object *obj) {
    // EE290CMachineState *s = (EE290CMachineState *)(obj);
}

static void machine_class_init(ObjectClass *oc, void *data) {
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "EE290C Oscibear SoC (sp21)";
    mc->init = machine_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = TYPE_RISCV_CPU_BASE;
    mc->default_ram_id = "riscv.sifive.e.ram";
    mc->default_ram_size = ee290c_oscibear_memmap[EE290C_OSCIBEAR_DEV_DTIM].size;
}

static const TypeInfo machine_typeinfo = {
    .name       = TYPE_EE290C_OSCIBEAR_MACHINE,
    .parent     = TYPE_MACHINE,
    .class_init = machine_class_init,
    .instance_init = machine_instance_init,
    .instance_size = sizeof(EE290CMachineState),
};

static void machine_init_register_types(void) {
    type_register_static(&machine_typeinfo);
}
type_init(machine_init_register_types)