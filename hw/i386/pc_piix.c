/*
 * QEMU PC System Emulator
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <glib.h>

#include "hw/hw.h"
#include "hw/loader.h"
#include "hw/i386/pc.h"
#include "hw/i386/apic.h"
#include "hw/i386/smbios.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "hw/usb.h"
#include "net/net.h"
#include "hw/boards.h"
#include "hw/ide.h"
#include "sysemu/kvm.h"
#include "hw/kvm/clock.h"
#include "sysemu/sysemu.h"
#include "hw/sysbus.h"
#include "hw/cpu/icc_bus.h"
#include "sysemu/arch_init.h"
#include "sysemu/block-backend.h"
#include "hw/i2c/smbus.h"
#include "hw/xen/xen.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "hw/acpi/acpi.h"
#include "cpu.h"
#include "qemu/error-report.h"
#ifdef CONFIG_XEN
#  include <xen/hvm/hvm_info_table.h>
#endif
#include "migration/migration.h"

#define MAX_IDE_BUS 2

static const int ide_iobase[MAX_IDE_BUS] = { 0x1f0, 0x170 };
static const int ide_iobase2[MAX_IDE_BUS] = { 0x3f6, 0x376 };
static const int ide_irq[MAX_IDE_BUS] = { 14, 15 };

static bool pci_enabled = true;
static bool has_acpi_build = true;
static bool rsdp_in_ram = true;
static int legacy_acpi_table_size;
static bool smbios_defaults = true;
static bool smbios_legacy_mode;
static bool smbios_uuid_encoded = true;
/* Make sure that guest addresses aligned at 1Gbyte boundaries get mapped to
 * host addresses aligned at 1Gbyte boundaries.  This way we can use 1GByte
 * pages in the host.
 */
static bool gigabyte_align = true;
static bool has_reserved_memory = true;
static bool kvmclock_enabled = true;

/* PC hardware initialisation */
static void pc_init1(MachineState *machine)
{
    PCMachineState *pc_machine = PC_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    MemoryRegion *system_io = get_system_io();
    int i;
    ram_addr_t below_4g_mem_size, above_4g_mem_size;
    PCIBus *pci_bus;
    ISABus *isa_bus;
    PCII440FXState *i440fx_state;
    int piix3_devfn = -1;
    qemu_irq *gsi;
    qemu_irq *i8259;
    qemu_irq smi_irq;
    GSIState *gsi_state;
    DriveInfo *hd[MAX_IDE_BUS * MAX_IDE_DEVS];
    BusState *idebus[MAX_IDE_BUS];
    ISADevice *rtc_state;
    MemoryRegion *ram_memory;
    MemoryRegion *pci_memory;
    MemoryRegion *rom_memory;
    DeviceState *icc_bridge;
    PcGuestInfo *guest_info;
    ram_addr_t lowmem;

    /* Check whether RAM fits below 4G (leaving 1/2 GByte for IO memory).
     * If it doesn't, we need to split it in chunks below and above 4G.
     * In any case, try to make sure that guest addresses aligned at
     * 1G boundaries get mapped to host addresses aligned at 1G boundaries.
     * For old machine types, use whatever split we used historically to avoid
     * breaking migration.
     */
    if (machine->ram_size >= 0xe0000000) {
        lowmem = gigabyte_align ? 0xc0000000 : 0xe0000000;
    } else {
        lowmem = 0xe0000000;
    }

    /* Handle the machine opt max-ram-below-4g.  It is basically doing
     * min(qemu limit, user limit).
     */
    if (lowmem > pc_machine->max_ram_below_4g) {
        lowmem = pc_machine->max_ram_below_4g;
        if (machine->ram_size - lowmem > lowmem &&
            lowmem & ((1ULL << 30) - 1)) {
            error_report("Warning: Large machine and max_ram_below_4g(%"PRIu64
                         ") not a multiple of 1G; possible bad performance.",
                         pc_machine->max_ram_below_4g);
        }
    }

    if (machine->ram_size >= lowmem) {
        above_4g_mem_size = machine->ram_size - lowmem;
        below_4g_mem_size = lowmem;
    } else {
        above_4g_mem_size = 0;
        below_4g_mem_size = machine->ram_size;
    }

    if (xen_enabled() && xen_hvm_init(&below_4g_mem_size, &above_4g_mem_size,
                                      &ram_memory) != 0) {
        fprintf(stderr, "xen hardware virtual machine initialisation failed\n");
        exit(1);
    }

    icc_bridge = qdev_create(NULL, TYPE_ICC_BRIDGE);
    object_property_add_child(qdev_get_machine(), "icc-bridge",
                              OBJECT(icc_bridge), NULL);

    pc_cpus_init(machine->cpu_model, icc_bridge);

    if (kvm_enabled() && kvmclock_enabled) {
        kvmclock_create();
    }

    if (pci_enabled) {
        pci_memory = g_new(MemoryRegion, 1);
        memory_region_init(pci_memory, NULL, "pci", UINT64_MAX);
        rom_memory = pci_memory;
    } else {
        pci_memory = NULL;
        rom_memory = system_memory;
    }

    guest_info = pc_guest_info_init(below_4g_mem_size, above_4g_mem_size);

    guest_info->has_acpi_build = has_acpi_build;
    guest_info->legacy_acpi_table_size = legacy_acpi_table_size;

    guest_info->isapc_ram_fw = !pci_enabled;
    guest_info->has_reserved_memory = has_reserved_memory;
    guest_info->rsdp_in_ram = rsdp_in_ram;

    if (smbios_defaults) {
        MachineClass *mc = MACHINE_GET_CLASS(machine);
        /* These values are guest ABI, do not change */
        smbios_set_defaults("QEMU", "Standard PC (i440FX + PIIX, 1996)",
                            mc->name, smbios_legacy_mode, smbios_uuid_encoded);
    }

    /* allocate ram and load rom/bios */
    if (!xen_enabled()) {
        pc_memory_init(machine, system_memory,
                       below_4g_mem_size, above_4g_mem_size,
                       rom_memory, &ram_memory, guest_info);
    } else if (machine->kernel_filename != NULL) {
        /* For xen HVM direct kernel boot, load linux here */
        xen_load_linux(machine->kernel_filename,
                       machine->kernel_cmdline,
                       machine->initrd_filename,
                       below_4g_mem_size,
                       guest_info);
    }

    gsi_state = g_malloc0(sizeof(*gsi_state));
    if (kvm_irqchip_in_kernel()) {
        kvm_pc_setup_irq_routing(pci_enabled);
        gsi = qemu_allocate_irqs(kvm_pc_gsi_handler, gsi_state,
                                 GSI_NUM_PINS);
    } else {
        gsi = qemu_allocate_irqs(gsi_handler, gsi_state, GSI_NUM_PINS);
    }

    if (pci_enabled) {
        pci_bus = i440fx_init(&i440fx_state, &piix3_devfn, &isa_bus, gsi,
                              system_memory, system_io, machine->ram_size,
                              below_4g_mem_size,
                              above_4g_mem_size,
                              pci_memory, ram_memory);
    } else {
        pci_bus = NULL;
        i440fx_state = NULL;
        isa_bus = isa_bus_new(NULL, get_system_memory(), system_io);
        no_hpet = 1;
    }
    isa_bus_irqs(isa_bus, gsi);

    if (kvm_irqchip_in_kernel()) {
        i8259 = kvm_i8259_init(isa_bus);
    } else if (xen_enabled()) {
        i8259 = xen_interrupt_controller_init();
    } else {
        i8259 = i8259_init(isa_bus, pc_allocate_cpu_irq());
    }

    for (i = 0; i < ISA_NUM_IRQS; i++) {
        gsi_state->i8259_irq[i] = i8259[i];
    }
    g_free(i8259);
    if (pci_enabled) {
        ioapic_init_gsi(gsi_state, "i440fx");
    }
    qdev_init_nofail(icc_bridge);

    pc_register_ferr_irq(gsi[13]);

    pc_vga_init(isa_bus, pci_enabled ? pci_bus : NULL);

    assert(pc_machine->vmport != ON_OFF_AUTO_MAX);
    if (pc_machine->vmport == ON_OFF_AUTO_AUTO) {
        pc_machine->vmport = xen_enabled() ? ON_OFF_AUTO_OFF : ON_OFF_AUTO_ON;
    }

    /* init basic PC hardware */
    pc_basic_device_init(isa_bus, gsi, &rtc_state, true,
                         (pc_machine->vmport != ON_OFF_AUTO_ON), 0x4);

    pc_nic_init(isa_bus, pci_bus);

    ide_drive_get(hd, ARRAY_SIZE(hd));
    if (pci_enabled) {
        PCIDevice *dev;
        if (xen_enabled()) {
            dev = pci_piix3_xen_ide_init(pci_bus, hd, piix3_devfn + 1);
        } else {
            dev = pci_piix3_ide_init(pci_bus, hd, piix3_devfn + 1);
        }
        idebus[0] = qdev_get_child_bus(&dev->qdev, "ide.0");
        idebus[1] = qdev_get_child_bus(&dev->qdev, "ide.1");
    } else {
        for(i = 0; i < MAX_IDE_BUS; i++) {
            ISADevice *dev;
            char busname[] = "ide.0";
            dev = isa_ide_init(isa_bus, ide_iobase[i], ide_iobase2[i],
                               ide_irq[i],
                               hd[MAX_IDE_DEVS * i], hd[MAX_IDE_DEVS * i + 1]);
            /*
             * The ide bus name is ide.0 for the first bus and ide.1 for the
             * second one.
             */
            busname[4] = '0' + i;
            idebus[i] = qdev_get_child_bus(DEVICE(dev), busname);
        }
    }

    pc_cmos_init(below_4g_mem_size, above_4g_mem_size, machine->boot_order,
                 machine, idebus[0], idebus[1], rtc_state);

    if (pci_enabled && usb_enabled()) {
        pci_create_simple(pci_bus, piix3_devfn + 2, "piix3-usb-uhci");
    }

    if (pci_enabled && acpi_enabled) {
        DeviceState *piix4_pm;
        I2CBus *smbus;

        smi_irq = qemu_allocate_irq(pc_acpi_smi_interrupt, first_cpu, 0);
        /* TODO: Populate SPD eeprom data.  */
        smbus = piix4_pm_init(pci_bus, piix3_devfn + 3, 0xb100,
                              gsi[9], smi_irq,
                              pc_machine_is_smm_enabled(pc_machine),
                              &piix4_pm);
        smbus_eeprom_init(smbus, 8, NULL, 0);

        object_property_add_link(OBJECT(machine), PC_MACHINE_ACPI_DEVICE_PROP,
                                 TYPE_HOTPLUG_HANDLER,
                                 (Object **)&pc_machine->acpi_dev,
                                 object_property_allow_set_link,
                                 OBJ_PROP_LINK_UNREF_ON_RELEASE, &error_abort);
        object_property_set_link(OBJECT(machine), OBJECT(piix4_pm),
                                 PC_MACHINE_ACPI_DEVICE_PROP, &error_abort);
    }

    if (pci_enabled) {
        pc_pci_device_init(pci_bus);
    }
}

static void pc_compat_2_3(MachineState *machine)
{
    PCMachineState *pcms = PC_MACHINE(machine);
    savevm_skip_section_footers();
    if (kvm_enabled()) {
        pcms->smm = ON_OFF_AUTO_OFF;
    }
    global_state_set_optional();
    savevm_skip_configuration();
}

static void pc_compat_2_2(MachineState *machine)
{
    pc_compat_2_3(machine);
    rsdp_in_ram = false;
    machine->suppress_vmdesc = true;
}

static void pc_compat_2_1(MachineState *machine)
{
    PCMachineState *pcms = PC_MACHINE(machine);

    pc_compat_2_2(machine);
    smbios_uuid_encoded = false;
    x86_cpu_compat_kvm_no_autodisable(FEAT_8000_0001_ECX, CPUID_EXT3_SVM);
    pcms->enforce_aligned_dimm = false;
}

static void pc_compat_2_0(MachineState *machine)
{
    pc_compat_2_1(machine);
    /* This value depends on the actual DSDT and SSDT compiled into
     * the source QEMU; unfortunately it depends on the binary and
     * not on the machine type, so we cannot make pc-i440fx-1.7 work on
     * both QEMU 1.7 and QEMU 2.0.
     *
     * Large variations cause migration to fail for more than one
     * consecutive value of the "-smp" maxcpus option.
     *
     * For small variations of the kind caused by different iasl versions,
     * the 4k rounding usually leaves slack.  However, there could be still
     * one or two values that break.  For QEMU 1.7 and QEMU 2.0 the
     * slack is only ~10 bytes before one "-smp maxcpus" value breaks!
     *
     * 6652 is valid for QEMU 2.0, the right value for pc-i440fx-1.7 on
     * QEMU 1.7 it is 6414.  For RHEL/CentOS 7.0 it is 6418.
     */
    legacy_acpi_table_size = 6652;
    smbios_legacy_mode = true;
    has_reserved_memory = false;
    pc_set_legacy_acpi_data_size();
}

static void pc_compat_1_7(MachineState *machine)
{
    pc_compat_2_0(machine);
    smbios_defaults = false;
    gigabyte_align = false;
    option_rom_has_mr = true;
    legacy_acpi_table_size = 6414;
    x86_cpu_compat_kvm_no_autoenable(FEAT_1_ECX, CPUID_EXT_X2APIC);
}

static void pc_compat_1_6(MachineState *machine)
{
    pc_compat_1_7(machine);
    rom_file_has_mr = false;
    has_acpi_build = false;
}

static void pc_compat_1_5(MachineState *machine)
{
    pc_compat_1_6(machine);
}

static void pc_compat_1_4(MachineState *machine)
{
    pc_compat_1_5(machine);
}

static void pc_compat_1_3(MachineState *machine)
{
    pc_compat_1_4(machine);
    enable_compat_apic_id_mode();
}

/* PC compat function for pc-0.14 to pc-1.2 */
static void pc_compat_1_2(MachineState *machine)
{
    pc_compat_1_3(machine);
    x86_cpu_compat_kvm_no_autoenable(FEAT_KVM, 1 << KVM_FEATURE_PV_EOI);
}

/* PC compat function for pc-0.10 to pc-0.13 */
static void pc_compat_0_13(MachineState *machine)
{
    pc_compat_1_2(machine);
    kvmclock_enabled = false;
}

static void pc_init_isa(MachineState *machine)
{
    pci_enabled = false;
    has_acpi_build = false;
    smbios_defaults = false;
    gigabyte_align = false;
    smbios_legacy_mode = true;
    has_reserved_memory = false;
    option_rom_has_mr = true;
    rom_file_has_mr = false;
    if (!machine->cpu_model) {
        machine->cpu_model = "486";
    }
    x86_cpu_compat_kvm_no_autoenable(FEAT_KVM, 1 << KVM_FEATURE_PV_EOI);
    enable_compat_apic_id_mode();
    pc_init1(machine);
}

#ifdef CONFIG_XEN
static void pc_xen_hvm_init(MachineState *machine)
{
    PCIBus *bus;

    pc_init1(machine);

    bus = pci_find_primary_bus();
    if (bus != NULL) {
        pci_create_simple(bus, -1, "xen-platform");
    }
}
#endif

#define DEFINE_I440FX_MACHINE(suffix, name, compatfn, optionfn) \
    static void pc_init_##suffix(MachineState *machine) \
    { \
        void (*compat)(MachineState *m) = (compatfn); \
        if (compat) { \
            compat(machine); \
        } \
        pc_init1(machine); \
    } \
    DEFINE_PC_MACHINE(suffix, name, pc_init_##suffix, optionfn)

static void pc_i440fx_machine_options(MachineClass *m)
{
    pc_default_machine_options(m);
    m->family = "pc_piix";
    m->desc = "Standard PC (i440FX + PIIX, 1996)";
    m->hot_add_cpu = pc_hot_add_cpu;
}

static void pc_i440fx_2_4_machine_options(MachineClass *m)
{
    pc_i440fx_machine_options(m);
    m->default_machine_opts = "firmware=bios-256k.bin";
    m->default_display = "std";
    m->alias = "pc";
    m->is_default = 1;
}

DEFINE_I440FX_MACHINE(v2_4, "pc-i440fx-2.4", NULL,
                      pc_i440fx_2_4_machine_options)


static void pc_i440fx_2_3_machine_options(MachineClass *m)
{
    pc_i440fx_2_4_machine_options(m);
    m->alias = NULL;
    m->is_default = 0;
    SET_MACHINE_COMPAT(m, PC_COMPAT_2_3);
}

DEFINE_I440FX_MACHINE(v2_3, "pc-i440fx-2.3", pc_compat_2_3,
                      pc_i440fx_2_3_machine_options);


static void pc_i440fx_2_2_machine_options(MachineClass *m)
{
    pc_i440fx_2_3_machine_options(m);
    SET_MACHINE_COMPAT(m, PC_COMPAT_2_2);
}

DEFINE_I440FX_MACHINE(v2_2, "pc-i440fx-2.2", pc_compat_2_2,
                      pc_i440fx_2_2_machine_options);


static void pc_i440fx_2_1_machine_options(MachineClass *m)
{
    pc_i440fx_2_2_machine_options(m);
    m->default_display = NULL;
    SET_MACHINE_COMPAT(m, PC_COMPAT_2_1);
}

DEFINE_I440FX_MACHINE(v2_1, "pc-i440fx-2.1", pc_compat_2_1,
                      pc_i440fx_2_1_machine_options);



static void pc_i440fx_2_0_machine_options(MachineClass *m)
{
    pc_i440fx_2_1_machine_options(m);
    SET_MACHINE_COMPAT(m, PC_COMPAT_2_0);
}

DEFINE_I440FX_MACHINE(v2_0, "pc-i440fx-2.0", pc_compat_2_0,
                      pc_i440fx_2_0_machine_options);


static void pc_i440fx_1_7_machine_options(MachineClass *m)
{
    pc_i440fx_2_0_machine_options(m);
    m->default_machine_opts = NULL;
    SET_MACHINE_COMPAT(m, PC_COMPAT_1_7);
}

DEFINE_I440FX_MACHINE(v1_7, "pc-i440fx-1.7", pc_compat_1_7,
                      pc_i440fx_1_7_machine_options);


static void pc_i440fx_1_6_machine_options(MachineClass *m)
{
    pc_i440fx_1_7_machine_options(m);
    SET_MACHINE_COMPAT(m, PC_COMPAT_1_6);
}

DEFINE_I440FX_MACHINE(v1_6, "pc-i440fx-1.6", pc_compat_1_6,
                      pc_i440fx_1_6_machine_options);


static void pc_i440fx_1_5_machine_options(MachineClass *m)
{
    pc_i440fx_1_6_machine_options(m);
    SET_MACHINE_COMPAT(m, PC_COMPAT_1_5);
}

DEFINE_I440FX_MACHINE(v1_5, "pc-i440fx-1.5", pc_compat_1_5,
                      pc_i440fx_1_5_machine_options);


static void pc_i440fx_1_4_machine_options(MachineClass *m)
{
    pc_i440fx_1_5_machine_options(m);
    m->hot_add_cpu = NULL;
    SET_MACHINE_COMPAT(m, PC_COMPAT_1_4);
}

DEFINE_I440FX_MACHINE(v1_4, "pc-i440fx-1.4", pc_compat_1_4,
                      pc_i440fx_1_4_machine_options);


#define PC_COMPAT_1_3 \
        PC_COMPAT_1_4 \
        {\
            .driver   = "usb-tablet",\
            .property = "usb_version",\
            .value    = stringify(1),\
        },{\
            .driver   = "virtio-net-pci",\
            .property = "ctrl_mac_addr",\
            .value    = "off",      \
        },{ \
            .driver   = "virtio-net-pci", \
            .property = "mq", \
            .value    = "off", \
        }, {\
            .driver   = "e1000",\
            .property = "autonegotiation",\
            .value    = "off",\
        },


static void pc_i440fx_1_3_machine_options(MachineClass *m)
{
    pc_i440fx_1_4_machine_options(m);
    SET_MACHINE_COMPAT(m, PC_COMPAT_1_3);
}

DEFINE_I440FX_MACHINE(v1_3, "pc-1.3", pc_compat_1_3,
                      pc_i440fx_1_3_machine_options);


#define PC_COMPAT_1_2 \
        PC_COMPAT_1_3 \
        {\
            .driver   = "nec-usb-xhci",\
            .property = "msi",\
            .value    = "off",\
        },{\
            .driver   = "nec-usb-xhci",\
            .property = "msix",\
            .value    = "off",\
        },{\
            .driver   = "ivshmem",\
            .property = "use64",\
            .value    = "0",\
        },{\
            .driver   = "qxl",\
            .property = "revision",\
            .value    = stringify(3),\
        },{\
            .driver   = "qxl-vga",\
            .property = "revision",\
            .value    = stringify(3),\
        },{\
            .driver   = "VGA",\
            .property = "mmio",\
            .value    = "off",\
        },

static void pc_i440fx_1_2_machine_options(MachineClass *m)
{
    pc_i440fx_1_3_machine_options(m);
    SET_MACHINE_COMPAT(m, PC_COMPAT_1_2);
}

DEFINE_I440FX_MACHINE(v1_2, "pc-1.2", pc_compat_1_2,
                      pc_i440fx_1_2_machine_options);


#define PC_COMPAT_1_1 \
        PC_COMPAT_1_2 \
        {\
            .driver   = "virtio-scsi-pci",\
            .property = "hotplug",\
            .value    = "off",\
        },{\
            .driver   = "virtio-scsi-pci",\
            .property = "param_change",\
            .value    = "off",\
        },{\
            .driver   = "VGA",\
            .property = "vgamem_mb",\
            .value    = stringify(8),\
        },{\
            .driver   = "vmware-svga",\
            .property = "vgamem_mb",\
            .value    = stringify(8),\
        },{\
            .driver   = "qxl-vga",\
            .property = "vgamem_mb",\
            .value    = stringify(8),\
        },{\
            .driver   = "qxl",\
            .property = "vgamem_mb",\
            .value    = stringify(8),\
        },{\
            .driver   = "virtio-blk-pci",\
            .property = "config-wce",\
            .value    = "off",\
        },

static void pc_i440fx_1_1_machine_options(MachineClass *m)
{
    pc_i440fx_1_2_machine_options(m);
    SET_MACHINE_COMPAT(m, PC_COMPAT_1_1);
}

DEFINE_I440FX_MACHINE(v1_1, "pc-1.1", pc_compat_1_2,
                      pc_i440fx_1_1_machine_options);


#define PC_COMPAT_1_0 \
        PC_COMPAT_1_1 \
        {\
            .driver   = TYPE_ISA_FDC,\
            .property = "check_media_rate",\
            .value    = "off",\
        }, {\
            .driver   = "virtio-balloon-pci",\
            .property = "class",\
            .value    = stringify(PCI_CLASS_MEMORY_RAM),\
        },{\
            .driver   = "apic-common",\
            .property = "vapic",\
            .value    = "off",\
        },{\
            .driver   = TYPE_USB_DEVICE,\
            .property = "full-path",\
            .value    = "no",\
        },

static void pc_i440fx_1_0_machine_options(MachineClass *m)
{
    pc_i440fx_1_1_machine_options(m);
    m->hw_version = "1.0";
    SET_MACHINE_COMPAT(m, PC_COMPAT_1_0);
}

DEFINE_I440FX_MACHINE(v1_0, "pc-1.0", pc_compat_1_2,
                      pc_i440fx_1_0_machine_options);


#define PC_COMPAT_0_15 \
        PC_COMPAT_1_0

static void pc_i440fx_0_15_machine_options(MachineClass *m)
{
    pc_i440fx_1_0_machine_options(m);
    m->hw_version = "0.15";
    SET_MACHINE_COMPAT(m, PC_COMPAT_0_15);
}

DEFINE_I440FX_MACHINE(v0_15, "pc-0.15", pc_compat_1_2,
                      pc_i440fx_0_15_machine_options);


#define PC_COMPAT_0_14 \
        PC_COMPAT_0_15 \
        {\
            .driver   = "virtio-blk-pci",\
            .property = "event_idx",\
            .value    = "off",\
        },{\
            .driver   = "virtio-serial-pci",\
            .property = "event_idx",\
            .value    = "off",\
        },{\
            .driver   = "virtio-net-pci",\
            .property = "event_idx",\
            .value    = "off",\
        },{\
            .driver   = "virtio-balloon-pci",\
            .property = "event_idx",\
            .value    = "off",\
        },{\
            .driver   = "qxl",\
            .property = "revision",\
            .value    = stringify(2),\
        },{\
            .driver   = "qxl-vga",\
            .property = "revision",\
            .value    = stringify(2),\
        },

static void pc_i440fx_0_14_machine_options(MachineClass *m)
{
    pc_i440fx_0_15_machine_options(m);
    m->hw_version = "0.14";
    SET_MACHINE_COMPAT(m, PC_COMPAT_0_14);
}

DEFINE_I440FX_MACHINE(v0_14, "pc-0.14", pc_compat_1_2,
                      pc_i440fx_0_14_machine_options);


#define PC_COMPAT_0_13 \
        PC_COMPAT_0_14 \
        {\
            .driver   = TYPE_PCI_DEVICE,\
            .property = "command_serr_enable",\
            .value    = "off",\
        },{\
            .driver   = "AC97",\
            .property = "use_broken_id",\
            .value    = stringify(1),\
        },{\
            .driver   = "virtio-9p-pci",\
            .property = "vectors",\
            .value    = stringify(0),\
        },{\
            .driver   = "VGA",\
            .property = "rombar",\
            .value    = stringify(0),\
        },{\
            .driver   = "vmware-svga",\
            .property = "rombar",\
            .value    = stringify(0),\
        },

static void pc_i440fx_0_13_machine_options(MachineClass *m)
{
    pc_i440fx_0_14_machine_options(m);
    m->hw_version = "0.13";
    SET_MACHINE_COMPAT(m, PC_COMPAT_0_13);
}

DEFINE_I440FX_MACHINE(v0_13, "pc-0.13", pc_compat_0_13,
                      pc_i440fx_0_13_machine_options);


#define PC_COMPAT_0_12 \
        PC_COMPAT_0_13 \
        {\
            .driver   = "virtio-serial-pci",\
            .property = "max_ports",\
            .value    = stringify(1),\
        },{\
            .driver   = "virtio-serial-pci",\
            .property = "vectors",\
            .value    = stringify(0),\
        },{\
            .driver   = "usb-mouse",\
            .property = "serial",\
            .value    = "1",\
        },{\
            .driver   = "usb-tablet",\
            .property = "serial",\
            .value    = "1",\
        },{\
            .driver   = "usb-kbd",\
            .property = "serial",\
            .value    = "1",\
        },

static void pc_i440fx_0_12_machine_options(MachineClass *m)
{
    pc_i440fx_0_13_machine_options(m);
    m->hw_version = "0.12";
    SET_MACHINE_COMPAT(m, PC_COMPAT_0_12);
}

DEFINE_I440FX_MACHINE(v0_12, "pc-0.12", pc_compat_0_13,
                      pc_i440fx_0_12_machine_options);


#define PC_COMPAT_0_11 \
        PC_COMPAT_0_12 \
        {\
            .driver   = "virtio-blk-pci",\
            .property = "vectors",\
            .value    = stringify(0),\
        },{\
            .driver   = TYPE_PCI_DEVICE,\
            .property = "rombar",\
            .value    = stringify(0),\
        },{\
            .driver   = "ide-drive",\
            .property = "ver",\
            .value    = "0.11",\
        },{\
            .driver   = "scsi-disk",\
            .property = "ver",\
            .value    = "0.11",\
        },

static void pc_i440fx_0_11_machine_options(MachineClass *m)
{
    pc_i440fx_0_12_machine_options(m);
    m->hw_version = "0.11";
    SET_MACHINE_COMPAT(m, PC_COMPAT_0_11);
}

DEFINE_I440FX_MACHINE(v0_11, "pc-0.11", pc_compat_0_13,
                      pc_i440fx_0_11_machine_options);


#define PC_COMPAT_0_10 \
    PC_COMPAT_0_11 \
    {\
        .driver   = "virtio-blk-pci",\
        .property = "class",\
        .value    = stringify(PCI_CLASS_STORAGE_OTHER),\
    },{\
        .driver   = "virtio-serial-pci",\
        .property = "class",\
        .value    = stringify(PCI_CLASS_DISPLAY_OTHER),\
    },{\
        .driver   = "virtio-net-pci",\
        .property = "vectors",\
        .value    = stringify(0),\
    },{\
        .driver   = "ide-drive",\
        .property = "ver",\
        .value    = "0.10",\
    },{\
        .driver   = "scsi-disk",\
        .property = "ver",\
        .value    = "0.10",\
    },

static void pc_i440fx_0_10_machine_options(MachineClass *m)
{
    pc_i440fx_0_11_machine_options(m);
    m->hw_version = "0.10";
    SET_MACHINE_COMPAT(m, PC_COMPAT_0_10);
}

DEFINE_I440FX_MACHINE(v0_10, "pc-0.10", pc_compat_0_13,
                      pc_i440fx_0_10_machine_options);


static void isapc_machine_options(MachineClass *m)
{
    pc_common_machine_options(m);
    m->desc = "ISA-only PC";
    m->max_cpus = 1;
}

DEFINE_PC_MACHINE(isapc, "isapc", pc_init_isa,
                  isapc_machine_options);


#ifdef CONFIG_XEN
static void xenfv_machine_options(MachineClass *m)
{
    pc_common_machine_options(m);
    m->desc = "Xen Fully-virtualized PC";
    m->max_cpus = HVM_MAX_VCPUS;
    m->default_machine_opts = "accel=xen";
    m->hot_add_cpu = pc_hot_add_cpu;
}

DEFINE_PC_MACHINE(xenfv, "xenfv", pc_xen_hvm_init,
                  xenfv_machine_options);
#endif
